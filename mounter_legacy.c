
// mounter_legacy.c — non-RDB media: MBR, extended/EBR chains, GPT,
// and superfloppies (a filesystem straight at block 0).
//
// Part of the generic autoboot/automount mounter; see mounter.c for the
// copyright and licence covering this file.
#include "mounter_internal.h"
#include "legacy.h"

// Classic recipe: FAT95 dostype, FileSystem.resource only.
static const struct MountFS defaultFatFS = { 0x46415401, NULL, NULL, NULL, 0, 0, 0, 0 };

/* classify_vbr() results */
#define VBR_NONE  0
#define VBR_FAT   1
#define VBR_NTFS  2
#define VBR_EXFAT 3

// Classify one block as a filesystem Volume Boot Record. A VBR ends in 0x55AA
// just like an MBR, so the BPB fields must be validated to tell a superfloppy
// (filesystem at block 0) from a partition table.
static int classify_vbr(const UBYTE *b, ULONG blocksize)
{
	if (b[510] != 0x55 || b[511] != 0xaa)
		return VBR_NONE;
	if (memcmp(b + 3, "NTFS    ", 8) == 0)
		return VBR_NTFS;
	if (memcmp(b + 3, "EXFAT   ", 8) == 0) {
		// exFAT keeps its geometry outside the FAT BPB, so validate the
		// fields the spec pins down instead: the MustBeZero region (the
		// bytes a FAT/NTFS BPB would fill), and the two shifts/counts the
		// filesystem itself refuses to mount without.
		for (int i = 11; i < 64; i++)
			if (b[i])
				return VBR_NONE;
		if (b[108] < 9 || b[108] > 12)          /* BytesPerSectorShift */
			return VBR_NONE;
		if (b[110] != 1 && b[110] != 2)         /* NumberOfFats */
			return VBR_NONE;
		return VBR_EXFAT;
	}
	if (b[0] != 0xeb && b[0] != 0xe9)          /* x86 jump opcode */
		return VBR_NONE;
	ULONG bps = (ULONG)(b[11] | (b[12] << 8));  /* BPB fields are little endian */
	UBYTE spc = b[13];
	if (bps != blocksize)
		return VBR_NONE;
	if (spc == 0 || (spc & (spc - 1)) != 0)    /* sectors/cluster: power of two */
		return VBR_NONE;
	if (b[16] != 1 && b[16] != 2)              /* number of FATs */
		return VBR_NONE;
	return VBR_FAT;
}

// Nothing this path mounts can ever be booted from, so every node it creates goes
// in at -128 ("don't even bother to boot from this device", per expansion.doc).
//
// classify_vbr() only ever answers FAT, NTFS or exFAT, and none of those carries an
// Amiga boot block — their VBR occupies the very sector strap would read one from.
// Passing the MBR active flag through as a boot priority only advertised these
// volumes as boot candidates that strap then silently skipped, and put them in the
// early-boot menu as if they were choices. If classify_vbr() ever learns to spot an
// Amiga filesystem in an MBR or GPT slot, this assumption has to come back.
static void register_legacy(struct MountData *md, UBYTE type, ULONG pstart, ULONG plen)
{
	(void)type; // In case we turned debugging off.

	const struct MountFS *fs = NULL;
	ULONG pend = pstart + plen - 1;

	if ((md->flags & MSF_LEGACY_FIRST_ONLY) && md->legacyMounted)
		return;

	// The MBR type byte / GPT type GUID only got us here; the partition's own
	// boot sector decides the filesystem (0x07 is NTFS or exFAT, sticks are
	// often mislabeled). Sniff it, in a buffer the caller's table doesn't use.
	UBYTE *vbr = mnt_sector_take(md);
	if (!vbr)
		return;
	int vbrType = mnt_read_block(vbr, pstart, 0xffffffff, md) ? classify_vbr(vbr, md->blocksize) : -1;
	mnt_sector_drop(md, vbr);
	if (vbrType < 0)
		return;
	/* classify_vbr() result -> recipe kind; VBR_NONE has none. */
	static const BYTE vbrKind[] = { -1, MOUNTFS_FAT, MOUNTFS_NTFS, MOUNTFS_EXFAT };
	if (vbrType > 0 && vbrType < (int)(sizeof(vbrKind) / sizeof(vbrKind[0]))) {
		fs = md->fs[vbrKind[vbrType]];
		/* FAT is the one family with a classic fallback when no recipe is given. */
		if (!fs && vbrKind[vbrType] == MOUNTFS_FAT)
			fs = &defaultFatFS;
	}
	if (!fs) {
		printf("Skipping partition at %lu (type 0x%02lx): unsupported filesystem\n",
		       pstart, (ULONG)type);
		return;
	}
	if (!mnt_resolve_fs(md, fs, pstart)) {
		return;
	}

	printf("register_legacy: %lu - %lu\n", pstart, pend);

	// Map the partition's block range straight to LowCyl/HighCyl (one block per
	// cylinder). The old CHS fitting rounded the start down and corrupted
	// unaligned partitions.
	struct Volume vol = {
		.fs       = fs,
		.nameHint = fs->dosName ? fs->dosName : (const UBYTE *)"MS0",
		.bootPri  = MOUNT_NEVER_BOOT,
	};
	mnt_envec_from_recipe(md, &vol.pp.de, fs, pstart, pend, MOUNT_NEVER_BOOT);

	if (mnt_mount_volume(md, &vol) != MOUNT_FAILED)
		md->legacyMounted = TRUE;
}

#define MAX_EXTENDED_PARTITIONS 16

// 0x05 = CHS extended, 0x0F = LBA extended (the common one), 0x85 = Linux extended.
static BOOL is_extended_type(UBYTE type)
{
	return type == 0x05 || type == 0x0f || type == 0x85;
}

// Walk the EBR chain of the extended partition starting at 'base'. Logical
// partition offsets are relative to their own EBR; the next-EBR link is
// relative to the extended container base. Returns partitions mounted.
static void parse_ebr(struct MountData *md, ULONG base)
{
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return;
	struct mbr *mbr = (struct mbr *)buf;
	ULONG ebr = base;
	int n;

	for (n = 0; n < MAX_EXTENDED_PARTITIONS; n++) {
		if (!mnt_read_block(buf, ebr, 0xffffffff, md))
			break;
		if (mbr->sig[0] != 0x55 || mbr->sig[1] != 0xaa)
			break;
		// Slot 0 is the logical partition, slot 1 links to the next EBR. Read
		// them straight from buf — register_legacy() below reads into its own
		// sector, so buf survives intact until the next iteration overwrites it.
		struct mbr_partition *logical = &mbr->part[0];
		struct mbr_partition *link    = &mbr->part[1];

		if (logical->type != 0 && logical->num_sect != 0) {
			printf("   %2ld   ", (LONG)(5 + n));
			printf("%lc   %02lx %8lx %8lx\n", (LONG)(logical->status & 0x80 ? '*':' '),
					(ULONG)logical->type,
					ebr + __bswap32(logical->f_lba),
					(ULONG)__bswap32(logical->num_sect));
			register_legacy(md, logical->type,
					ebr + __bswap32(logical->f_lba),
					__bswap32(logical->num_sect));
		}

		if (!is_extended_type(link->type) || link->num_sect == 0)
			break;
		ebr = base + __bswap32(link->f_lba);
	}
	if (n == MAX_EXTENDED_PARTITIONS) {
		printf("Warning: Extended partition limit (%ld) reached\n",
		       (LONG)MAX_EXTENDED_PARTITIONS);
	}
	mnt_sector_drop(md, buf);
}

// Any 0xEE slot marks the disk GPT, whether protective (covers the whole
// disk) or hybrid (real entries alongside). Per UEFI spec this gates GPT.
static BOOL has_protective_entry(const struct mbr *mbr)
{
	for (int i = 0; i < 4; i++) {
		if (mbr->part[i].type == 0xee)
			return TRUE;
	}
	return FALSE;
}

// CRC-32 (reflected, poly 0xEDB88320), bitwise: only ever run over one ~92-byte
// header, not worth a table.
static ULONG crc32(const UBYTE *p, ULONG len)
{
	ULONG crc = 0xFFFFFFFF;
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
	}
	return ~crc;
}

// UEFI-spec GPT header check: signature, the header must claim to live at the
// LBA it was read from, sane size, and the self-CRC32. This is what tells a
// live GPT from a stale header left at block 1 by an earlier repartitioning.
// (The entry-array CRC is skipped; parse_gpt bounds-checks what it reads.)
static BOOL is_valid_gpt_header(struct gpt *gpt, struct MountData *md)
{
	if (memcmp(gpt->signature, "EFI PART", 8) != 0)
		return FALSE;
	if (__bswap64(gpt->my_lba) != 1)
		return FALSE;
	ULONG size = __bswap32(gpt->size);
	if (size < sizeof(struct gpt) || size > (ULONG)md->blocksize)
		return FALSE;
	ULONG stored = gpt->header_crc32;
	gpt->header_crc32 = 0;   /* the CRC is computed with its own field zeroed */
	ULONG computed = crc32((const UBYTE *)gpt, size);
	gpt->header_crc32 = stored;
	return computed == __bswap32(stored);
}

// Reject boot-code garbage that happens to end in 0x55AA (a filesystem VBR does,
// too): every slot must look like a real entry and at least one must be usable.
static BOOL is_sane_mbr(const struct mbr *mbr, ULONG total)
{
	int used = 0;
	for (int i = 0; i < 4; i++) {
		const struct mbr_partition *p = &mbr->part[i];
		if (p->status & 0x7f)
			return FALSE;
		if (p->type == 0 || p->f_lba == 0 || p->num_sect == 0)
			continue;
		if (total && __bswap32(p->f_lba) >= total)
			return FALSE;
		used++;
	}
	return used > 0;
}

static void parse_mbr(UBYTE *buf, struct MountData *md)
{
	struct mbr *mbr = (struct mbr *)buf;

	printf(" Part Boot Type   Start   Length\n");
	// The mount helpers below read into their own sectors, so buf (the MBR)
	// stays valid across the loop — index the four entries in place.
	for (int i = 0; i < 4; i++) {
		struct mbr_partition *p = &mbr->part[i];
		if (p->type == 0 || p->f_lba == 0 || p->num_sect == 0) {
			continue;
		}
		printf("   %2ld   ", (LONG)(i+1));
		printf("%lc   %02lx %8lx %8lx\n", (LONG)(p->status & 0x80 ? '*':' '),
			(ULONG)p->type,
			(ULONG)__bswap32(p->f_lba),
			(ULONG)__bswap32(p->num_sect));

		if (is_extended_type(p->type)) {
			parse_ebr(md, __bswap32(p->f_lba));
		} else if (p->type == 0xee) {
			// GPT protective entry; GPT itself was already probed at block 1.
		} else {
			register_legacy(md, p->type,
					__bswap32(p->f_lba),
					__bswap32(p->num_sect));
		}
	}
}

static void print_guid(GUID *x)
{
	(void)x; // In case we turned debugging off.

	// Somebody has got to be proud of this mixed endian prank.

	printf("%08lx-%04lx-%04lx-%02lx%02lx-%02lx%02lx%02lx%02lx%02lx%02lx",
			(ULONG)__bswap32(x->u.UUID.time_low), (ULONG)__bswap16(x->u.UUID.time_mid),
			(ULONG)__bswap16(x->u.UUID.time_high_and_version),
			(ULONG)x->u.UUID.clock_seq_high_and_reserved, (ULONG)x->u.UUID.clock_seq_low,
			(ULONG)x->u.UUID.node[0], (ULONG)x->u.UUID.node[1], (ULONG)x->u.UUID.node[2],
			(ULONG)x->u.UUID.node[3], (ULONG)x->u.UUID.node[4], (ULONG)x->u.UUID.node[5]);
}

// Microsoft Basic Data Partition GUID (used for FAT, exFAT and NTFS),
// EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
static const GUID GUID_BASIC_DATA = {{
	.raw = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
	         0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 }
}};

static int guid_equal(const GUID *a, const GUID *b)
{
	return memcmp(a->u.raw, b->u.raw, 16) == 0;
}

// Walk the GPT entry array, mounting Basic Data partitions. Returns FALSE if the
// table could not be read at all — the caller treats that as "not a GPT after all"
// and falls back to the MBR. Mounting nothing is still TRUE: an empty GPT is a GPT.
static BOOL parse_gpt(UBYTE *hdr, struct MountData *md)
{
	const struct gpt *gpt=(struct gpt *)hdr;
	uint64_t pstart = __bswap64(gpt->entries_lba);
	ULONG numparts = __bswap32(gpt->number_of_entries);
	const ULONG psize = __bswap32(gpt->size_of_entry);

	printf(" Number of partitions: %lu\n", numparts);
	printf(" size of entry: %lu\n", psize);

	if (psize < sizeof(struct gpt_partition) || psize > md->blocksize)
		return FALSE;
	if (numparts > 128)   /* standard table size; don't chase garbage counts */
		numparts = 128;
	const ULONG per_block = md->blocksize / psize;

	// The partition-entry array is read into its own sector; the header stays
	// in 'hdr' (its fields are still needed) and block 0 is left untouched.
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return FALSE;

	for (ULONG i = 0, pos = 0; i < numparts; i++) {
		if (pos == 0) {
			if (!mnt_read_block(buf, (ULONG)pstart++, 0xffffffff, md))
				break;
		}
		struct gpt_partition *gpt_par = (struct gpt_partition *)(buf + (pos * psize));
		if (++pos == per_block)
			pos = 0;

		/* skip empty partitions */
		if (gpt_par->first_lba == 0 && gpt_par->last_lba == 0)
			continue;

		const uint64_t first_lba = __bswap64(gpt_par->first_lba);
		const uint64_t last_lba = __bswap64(gpt_par->last_lba);

		printf("%lu. %08lx%08lx - %08lx%08lx ", i,
				(ULONG)(first_lba >> 32), (ULONG)first_lba,
				(ULONG)(last_lba >> 32), (ULONG)last_lba);
		print_guid(&gpt_par->partition_type);
		printf("\n");

		if (!guid_equal(&gpt_par->partition_type, &GUID_BASIC_DATA)) {
			printf("   Skipping partition: not a Basic Data type\n");
			continue;
		}
		if ((last_lba >> 32) != 0) {
			printf("   Skipping partition beyond 2^32 blocks\n");
			continue;
		}
		register_legacy(md, 0, (ULONG)first_lba,
				(ULONG)(last_lba - first_lba + 1));
	}

	mnt_sector_drop(md, buf);
	return TRUE;
}

// Non-RDB media, classified from one read of block 0: GPT (gated by its
// protective/hybrid MBR entry, so a stale GPT header on a since-repartitioned
// disk can't override the current table), then a filesystem straight at
// block 0 (superfloppy — its VBR ends in 0x55AA too, so it must be checked
// before the MBR signature), then MBR.
// Returns TRUE if the medium was recognized, whether or not anything mounted;
// the count is in md->mounted.
BOOL mnt_scan_legacy(struct MountData *md)
{
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return FALSE;
	BOOL recognized = FALSE;

	if (mnt_read_block(buf, 0, 0xffffffff, md)) {
		struct mbr *mbr = (struct mbr *)buf;
		BOOL hasMBRSig = mbr->sig[0] == 0x55 && mbr->sig[1] == 0xaa;

		if (hasMBRSig && has_protective_entry(mbr)) {
			// The GPT header gets its own sector so block 0 stays intact for the
			// MBR fallback if the header is invalid. A valid header commits us to
			// GPT — parse_gpt's result stands even when it mounts nothing.
			UBYTE *hdr = mnt_sector_take(md);
			BOOL gptValid = FALSE;
			if (hdr) {
				if (mnt_read_block(hdr, 1, 0xffffffff, md) &&
				    is_valid_gpt_header((struct gpt *)hdr, md)) {
					dbg("GPT found\n");
					gptValid = TRUE;
					recognized = parse_gpt(hdr, md);
				}
				mnt_sector_drop(md, hdr);
			}
			if (gptValid)
				goto done;
			// Fall through: a hybrid MBR may still carry mountable entries; a
			// pure protective one fails is_sane_mbr or mounts nothing in parse_mbr.
			printf("GPT protective entry but no valid GPT header at block 1\n");
		}

		if (classify_vbr(buf, md->blocksize) != VBR_NONE) {
			if (md->totalsectors) {
				dbg("Superfloppy (no partition table)\n");
				register_legacy(md, 0, 0, md->totalsectors);
				recognized = TRUE;
			}
		} else if (hasMBRSig && is_sane_mbr(mbr, md->totalsectors)) {
			dbg("MBR found\n");
			parse_mbr(buf, md);
			recognized = TRUE;
		}
	}

done:
	mnt_sector_drop(md, buf);
	return recognized;
}
