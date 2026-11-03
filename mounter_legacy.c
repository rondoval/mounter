
// mounter_legacy.c — non-RDB media: MBR, extended/EBR chains, GPT,
// and superfloppies (a filesystem straight at block 0). One walker reads the
// table; mounting and the virtual-drive scan (mnt_find_containers) are its
// two callbacks.
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

/**
 * classify_vbr() - Classify one block as a filesystem Volume Boot Record.
 * @b:         the block, in a buffer of at least 512 bytes (bytes 0..511 are read)
 * @blocksize: the medium's block size in bytes; a FAT BPB's bytes per sector must
 *             equal it
 *
 * A VBR ends in 0x55AA just like an MBR, so the BPB fields must be validated to tell
 * a superfloppy (filesystem at block 0) from a partition table. NTFS is known by its
 * OEM ID alone.
 *
 * Return: VBR_FAT, VBR_NTFS or VBR_EXFAT for a VBR of that family; VBR_NONE for
 * anything else, an MBR included
 */
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

/**
 * register_legacy() - Mount one FAT, NTFS or exFAT partition, or a superfloppy.
 * @md:     the unit being scanned
 * @type:   the MBR type byte, for the log only; 0 for a GPT entry or a superfloppy
 * @pstart: the first block
 * @plen:   the length in blocks, nonzero; @pstart + @plen - 1 must not wrap
 *
 * The partition's own first block picks the recipe: classify_vbr() names the
 * family, md->fs[] its MountFS, and FAT alone falls back to defaultFatFS. The extent
 * maps to de_LowCyl..de_HighCyl at one block per cylinder. Skipped without a node:
 * anything after the first mount under MSF_LEGACY_FIRST_ONLY, no free scratch
 * sector, an unreadable first block, an unknown filesystem, a recipe
 * mnt_resolve_fs() refuses. Any mnt_mount_volume() result but MOUNT_FAILED sets
 * md->legacyMounted.
 *
 * Nothing this path mounts can ever be booted from, so every node it creates goes
 * in at -128 ("don't even bother to boot from this device", per expansion.doc).
 *
 * classify_vbr() only ever answers FAT, NTFS or exFAT, and none of those carries an
 * Amiga boot block — their VBR occupies the very sector strap would read one from.
 * Passing the MBR active flag through as a boot priority only advertised these
 * volumes as boot candidates that strap then silently skipped, and put them in the
 * early-boot menu as if they were choices. If classify_vbr() ever learns to spot an
 * Amiga filesystem in an MBR or GPT slot, this assumption has to come back.
 */
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

// One partition as the walker found it.
struct TableEntry
{
	UBYTE mbrType;              /* MBR/EBR type byte; 0 for a GPT entry */
	const GUID *gptType;        /* GPT type GUID; NULL for an MBR entry */
	uint64_t start;             /* first block */
	uint64_t length;            /* in blocks, at least 1 */
};

// Called by walk_table() once per partition, in table order. te, and the GUID its
// gptType points to, are valid only during the call; ctx is walk_table()'s.
typedef void (*entry_fn)(struct MountData *md, const struct TableEntry *te, APTR ctx);

// MBR type of a virtual drive: a whole RDB disk inside the partition.
#define MBR_TYPE_VIRTUAL_DRIVE 0x76

// Emu68's GPT type of the same, 3F82EEBC-87C9-4097-8165-89D6540557C0.
static const GUID GUID_VIRTUAL_DRIVE = {{
	.raw = { 0xBC, 0xEE, 0x82, 0x3F, 0xC9, 0x87, 0x97, 0x40,
	         0x81, 0x65, 0x89, 0xD6, 0x54, 0x05, 0x57, 0xC0 }
}};

// Microsoft Basic Data Partition GUID (used for FAT, exFAT and NTFS),
// EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
static const GUID GUID_BASIC_DATA = {{
	.raw = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
	         0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 }
}};

/**
 * guid_equal() - Compare two GUIDs byte for byte.
 * @a: one GUID, in on-disk byte order
 * @b: the other, in the same order
 *
 * Return: TRUE if all 16 bytes match, FALSE otherwise
 */
static BOOL guid_equal(const GUID *a, const GUID *b)
{
	return memcmp(a->u.raw, b->u.raw, 16) == 0;
}

/**
 * is_virtual_drive() - Tell whether a partition is a virtual drive.
 * @te: the partition
 *
 * Return: TRUE for an MBR/EBR entry of type MBR_TYPE_VIRTUAL_DRIVE (0x76) or a GPT
 * entry of type GUID_VIRTUAL_DRIVE; FALSE otherwise
 */
static BOOL is_virtual_drive(const struct TableEntry *te)
{
	return te->gptType ? guid_equal(te->gptType, &GUID_VIRTUAL_DRIVE)
	                   : te->mbrType == MBR_TYPE_VIRTUAL_DRIVE;
}

/**
 * mount_entry() - Mount one partition: the entry_fn of mnt_scan_legacy().
 * @md:  the unit being scanned
 * @te:  the partition, valid for this call only
 * @ctx: unused; mnt_scan_legacy() passes NULL
 *
 * A virtual drive belongs to the host's virtual device and is skipped before its
 * first block is ever sniffed: a new one may still carry the FAT boot sector of
 * whatever the partition held before. A GPT entry that is not GUID_BASIC_DATA is
 * skipped too, and so is a partition whose last block lies at 2^32 or beyond
 * (register_legacy() takes ULONG blocks); the rest go to register_legacy().
 */
static void mount_entry(struct MountData *md, const struct TableEntry *te, APTR ctx)
{
	(void)ctx;
	if (is_virtual_drive(te)) {
		printf("   Virtual drive, left to the virtual device\n");
		return;
	}
	if (te->gptType && !guid_equal(te->gptType, &GUID_BASIC_DATA)) {
		printf("   Skipping partition: not a Basic Data type\n");
		return;
	}
	if (((te->start + te->length - 1) >> 32) != 0) {
		printf("   Skipping partition beyond 2^32 blocks\n");
		return;
	}
	register_legacy(md, te->mbrType, (ULONG)te->start, (ULONG)te->length);
}

/**
 * is_extended_type() - Tell whether an MBR type byte marks an extended partition.
 * @type: the MBR/EBR type byte
 *
 * 0x05 = CHS extended, 0x0F = LBA extended (the common one), 0x85 = extended
 * (alternative ID).
 *
 * Return: TRUE for 0x05, 0x0F and 0x85; FALSE for any other type
 */
static BOOL is_extended_type(UBYTE type)
{
	return type == 0x05 || type == 0x0f || type == 0x85;
}

/**
 * parse_ebr() - Hand each logical partition of an extended partition to @fn.
 * @md:   the unit being scanned
 * @base: the extended partition's first block (its MBR slot's f_lba), the first EBR
 * @fn:   called once per logical partition whose type and num_sect are nonzero
 * @ctx:  passed through to @fn
 *
 * Walks the EBR chain. Logical partition offsets are relative to their own EBR; the
 * next-EBR link is relative to the extended container base. The walk ends at an
 * unreadable EBR, one without the 0x55AA signature, a link slot that is not
 * is_extended_type() or has num_sect 0, or after MAX_EXTENDED_PARTITIONS EBRs,
 * which also bounds a chain that loops. Without a free scratch sector nothing is
 * walked.
 */
static void parse_ebr(struct MountData *md, ULONG base, entry_fn fn, APTR ctx)
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
		// them straight from buf — the callback reads into its own sectors, so
		// buf survives intact until the next iteration overwrites it.
		struct mbr_partition *logical = &mbr->part[0];
		struct mbr_partition *link    = &mbr->part[1];

		if (logical->type != 0 && logical->num_sect != 0) {
			printf("   %2ld   ", (LONG)(5 + n));
			printf("%lc   %02lx %8lx %8lx\n", (LONG)(logical->status & 0x80 ? '*':' '),
					(ULONG)logical->type,
					ebr + __bswap32(logical->f_lba),
					(ULONG)__bswap32(logical->num_sect));
			const struct TableEntry te = {
				.mbrType = logical->type,
				.start   = (uint64_t)ebr + __bswap32(logical->f_lba),
				.length  = __bswap32(logical->num_sect),
			};
			fn(md, &te, ctx);
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

/**
 * has_protective_entry() - Tell whether an MBR has a GPT protective (0xEE) slot.
 * @mbr: block 0, its 0x55AA signature already checked
 *
 * Any 0xEE slot marks the disk GPT, whether protective (covers the whole disk) or
 * hybrid (real entries alongside). Per UEFI spec this gates GPT.
 *
 * Return: TRUE if any of the four slots has type 0xEE, FALSE otherwise
 */
static BOOL has_protective_entry(const struct mbr *mbr)
{
	for (int i = 0; i < 4; i++) {
		if (mbr->part[i].type == 0xee)
			return TRUE;
	}
	return FALSE;
}

/**
 * crc32() - Compute the CRC-32 of a buffer.
 * @p:   the data
 * @len: its length in bytes
 *
 * Reflected, poly 0xEDB88320, initial value and final XOR 0xFFFFFFFF; bitwise: only
 * ever run over one ~92-byte header, not worth a table.
 *
 * Return: the CRC as a number (the GPT header stores it little-endian)
 */
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

/**
 * is_valid_gpt_header() - Check the GPT header read from block 1.
 * @gpt: the header, in a sector of md->blocksize bytes; writable, as header_crc32
 *       is zeroed for the CRC and then restored
 * @md:  the unit; md->blocksize bounds the header size
 *
 * UEFI-spec checks: signature "EFI PART", my_lba 1 (the header must claim to live
 * at the LBA it was read from), size from sizeof(struct gpt) up to the block size,
 * and the self-CRC32. This is what tells a live GPT from a stale header left at
 * block 1 by an earlier repartitioning. The entry-array CRC is skipped; parse_gpt()
 * bounds-checks what it reads.
 *
 * Return: TRUE if every check passes, FALSE otherwise
 */
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

/**
 * is_sane_mbr() - Check that block 0 holds a believable MBR partition table.
 * @mbr:   block 0, its 0x55AA signature already checked
 * @total: the unit's size in blocks, which every used slot must start below; 0 if
 *         unknown, for no bound
 *
 * Rejects boot-code garbage that happens to end in 0x55AA (a filesystem VBR does,
 * too): every slot must look like a real entry and at least one must be usable. A
 * slot whose type, f_lba or num_sect is 0 is unused.
 *
 * Return: TRUE if every slot's status is 0x00 or 0x80, every used slot starts below
 * @total and at least one is used; FALSE otherwise
 */
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

/**
 * parse_mbr() - Hand each partition of an MBR and its EBR chains to @fn.
 * @buf: block 0, already passed by is_sane_mbr(); read in place, so it must stay
 *       valid until the call returns
 * @md:  the unit being scanned
 * @fn:  called once per used primary slot and per logical partition, in table order
 * @ctx: passed through to @fn
 *
 * A slot whose type, f_lba or num_sect is 0 is unused. An extended slot
 * (is_extended_type()) goes to parse_ebr(), its logical partitions logged from 5
 * on; a 0xEE slot (a hybrid MBR's GPT protective entry) is passed over, since
 * walk_table() has already probed the GPT.
 */
static void parse_mbr(UBYTE *buf, struct MountData *md, entry_fn fn, APTR ctx)
{
	struct mbr *mbr = (struct mbr *)buf;

	printf(" Part Boot Type   Start   Length\n");
	// The callbacks read into their own sectors, so buf (the MBR) stays valid
	// across the loop — index the four entries in place.
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
			parse_ebr(md, __bswap32(p->f_lba), fn, ctx);
		} else if (p->type == 0xee) {
			// GPT protective entry; GPT itself was already probed at block 1.
		} else {
			const struct TableEntry te = {
				.mbrType = p->type,
				.start   = __bswap32(p->f_lba),
				.length  = __bswap32(p->num_sect),
			};
			fn(md, &te, ctx);
		}
	}
}

/**
 * print_guid() - Log a GUID in its canonical 8-4-4-4-12 text form.
 * @x: the GUID, in on-disk byte order (the first three fields little-endian)
 *
 * Without MOUNTER_LOG the printf() compiles away.
 */
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

/**
 * parse_gpt() - Walk the GPT entry array, handing every used entry to @fn.
 * @hdr: the GPT header, validated by is_valid_gpt_header(); its fields are read
 *       throughout, so it must stay valid until the call returns
 * @md:  the unit being scanned
 * @fn:  called once per used entry, in array order
 * @ctx: passed through to @fn
 *
 * Reads number_of_entries entries (at most 128) of size_of_entry bytes from
 * entries_lba on, a 64-bit block number. An entry with first_lba and last_lba both 0 is unused; one whose
 * last_lba is below its first_lba is skipped. An unreadable block of the array ends
 * the walk.
 *
 * FALSE makes walk_table() report no table: a valid header has already committed it
 * to GPT, so the MBR is not walked. An empty GPT is still a GPT: TRUE.
 *
 * Return: FALSE if size_of_entry is below sizeof(struct gpt_partition) or above the
 * block size, or no scratch sector is free; TRUE otherwise, an empty table or a
 * walk cut short by an unreadable block included
 */
static BOOL parse_gpt(UBYTE *hdr, struct MountData *md, entry_fn fn, APTR ctx)
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
			if (!mnt_read_block(buf, pstart++, 0xffffffff, md))
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

		if (last_lba < first_lba) {
			printf("   Skipping partition that ends before it starts\n");
			continue;
		}
		const struct TableEntry te = {
			.gptType = &gpt_par->partition_type,
			.start   = first_lba,
			.length  = last_lba - first_lba + 1,
		};
		fn(md, &te, ctx);
	}

	mnt_sector_drop(md, buf);
	return TRUE;
}

/**
 * walk_table() - Walk the partition table at block 0, handing every entry to @fn.
 * @md:  the unit: md->SysBase, md->request, md->blocksize, md->totalsectors set
 * @fn:  called once per partition, in table order
 * @ctx: passed through to @fn
 * @vbr: out: classify_vbr() of block 0; VBR_NONE when the result is -1 or a valid
 *       GPT header was found
 *
 * GPT (gated by its protective/hybrid MBR entry, so a stale GPT header on a
 * since-repartitioned disk can't override the current table), else MBR with its EBR
 * chains. A filesystem straight at block 0 is no table; its VBR ends in 0x55AA too,
 * so it is checked before the MBR signature and reported through @vbr. While @fn
 * runs, up to three scratch sectors are held: block 0, the GPT header, the entry
 * array.
 *
 * Return: 1 if a GPT or an MBR was walked; 0 if block 0 holds no table (a VBR, no
 * 0x55AA, an MBR that fails is_sane_mbr(), a block under 512 bytes) or a valid
 * GPT header whose entry array parse_gpt() refused; -1 if block 0 cannot be read
 * or no scratch sector is free
 */
static int walk_table(struct MountData *md, entry_fn fn, APTR ctx, int *vbr)
{
	*vbr = VBR_NONE;
	// An MBR, a GPT header and a VBR each take 512 bytes: a smaller block holds none.
	if (md->blocksize < 512)
		return 0;
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return -1;
	int found = -1;

	if (mnt_read_block(buf, 0, 0xffffffff, md)) {
		struct mbr *mbr = (struct mbr *)buf;
		BOOL hasMBRSig = mbr->sig[0] == 0x55 && mbr->sig[1] == 0xaa;
		found = 0;

		if (hasMBRSig && has_protective_entry(mbr)) {
			// The GPT header gets its own sector so block 0 stays intact for the
			// MBR fallback if the header is invalid. A valid header commits us to
			// GPT — parse_gpt's result stands even when it yields nothing.
			UBYTE *hdr = mnt_sector_take(md);
			BOOL gptValid = FALSE;
			if (hdr) {
				if (mnt_read_block(hdr, 1, 0xffffffff, md) &&
				    is_valid_gpt_header((struct gpt *)hdr, md)) {
					dbg("GPT found\n");
					gptValid = TRUE;
					found = parse_gpt(hdr, md, fn, ctx) ? 1 : 0;
				}
				mnt_sector_drop(md, hdr);
			}
			if (gptValid)
				goto done;
			// Fall through: a hybrid MBR may still carry entries; a pure
			// protective one fails is_sane_mbr or yields nothing in parse_mbr.
			printf("GPT protective entry but no valid GPT header at block 1\n");
		}

		*vbr = classify_vbr(buf, md->blocksize);
		if (*vbr == VBR_NONE && hasMBRSig && is_sane_mbr(mbr, md->totalsectors)) {
			dbg("MBR found\n");
			parse_mbr(buf, md, fn, ctx);
			found = 1;
		}
	}

done:
	mnt_sector_drop(md, buf);
	return found;
}

/**
 * mnt_scan_legacy() - Mount a non-RDB medium's FAT, NTFS and exFAT volumes.
 * @md: the unit being scanned, as probe_unit() set it up
 *
 * The partition table's volumes (mount_entry()), or, with no table but a VBR at
 * block 0 and a nonzero md->totalsectors, the whole medium as a superfloppy. The
 * count is in md->mounted.
 *
 * Return: TRUE if the medium was recognized (a table walked, or a superfloppy),
 * whether or not anything mounted; FALSE otherwise, block 0 unreadable included
 */
BOOL mnt_scan_legacy(struct MountData *md)
{
	int vbr;
	int found = walk_table(md, mount_entry, NULL, &vbr);
	if (found > 0)
		return TRUE;
	if (found == 0 && vbr != VBR_NONE && md->totalsectors) {
		dbg("Superfloppy (no partition table)\n");
		register_legacy(md, 0, 0, md->totalsectors);
		return TRUE;
	}
	return FALSE;
}

struct ContainerScan
{
	struct MountContainer *out;
	ULONG max;
	ULONG found;
};

/**
 * collect_container() - Record a partition if it is a virtual drive.
 * @md:  unused
 * @te:  the partition, valid for this call only
 * @ctx: the struct ContainerScan; its out has room for max entries
 *
 * The entry_fn of mnt_find_containers(). Every virtual drive counts in found; its
 * start and length (in blocks) are written to out[found] only while found is below
 * max.
 */
static void collect_container(struct MountData *md, const struct TableEntry *te, APTR ctx)
{
	(void)md;
	struct ContainerScan *cs = ctx;
	if (!is_virtual_drive(te))
		return;
	if (cs->found < cs->max) {
		cs->out[cs->found].start = te->start;
		cs->out[cs->found].length = te->length;
	}
	cs->found++;
}

/**
 * mnt_find_containers() - List the virtual drives in a unit's partition table.
 * @SysBase:      exec
 * @request:      an IOExtTD open on the unit; DoIO()'d with CMD_READ/TD_READ64, so
 *                it must be the calling task's
 * @blockSize:    the unit's block size in bytes, 256..MAX_BLOCKSIZE
 * @totalSectors: the unit's size in blocks, bounding is_sane_mbr(); 0 for no bound
 * @out:          room for @max entries; filled with start and length in blocks, in
 *                table order
 * @max:          the entries @out holds
 *
 * Needs no MountDrive() session: walk_table() runs on a zeroed MountData of its own,
 * whose scratch sectors are freed before the call returns.
 *
 * Return: the number of virtual drives in the table, which may be more than @max
 * (only the first @max are written), 0 if there is no table; -1 if @blockSize is
 * out of range, memory runs out or block 0 cannot be read
 */
LONG mnt_find_containers(struct ExecBase *SysBase, struct IOExtTD *request,
                         ULONG blockSize, ULONG totalSectors,
                         struct MountContainer *out, ULONG max)
{
	if (blockSize < 256 || blockSize > MAX_BLOCKSIZE)
		return -1;
	struct MountData *md = AllocMem(sizeof(struct MountData), MEMF_PUBLIC | MEMF_CLEAR);
	if (!md)
		return -1;
	md->SysBase = SysBase;
	md->request = request;
	md->blocksize = blockSize;
	md->totalsectors = totalSectors;

	struct ContainerScan cs = { out, max, 0 };
	int vbr;
	int found = walk_table(md, collect_container, &cs, &vbr);

	mnt_sector_pool_free(md);
	FreeMem(md, sizeof(struct MountData));
	return found < 0 ? -1 : (LONG)cs.found;
}

struct BlockProbe
{
	uint64_t block;
	BOOL inside;
};

/**
 * probe_container() - Flag the probed block if this virtual drive covers it.
 * @md:  unused
 * @te:  the partition, valid for this call only
 * @ctx: the struct BlockProbe; its block is the block asked about
 *
 * The entry_fn of mnt_in_container(). Sets inside to TRUE when @te is a virtual
 * drive whose start..start + length - 1 covers block; never clears it.
 */
static void probe_container(struct MountData *md, const struct TableEntry *te, APTR ctx)
{
	(void)md;
	struct BlockProbe *bp = ctx;
	if (is_virtual_drive(te) && bp->block >= te->start && bp->block - te->start < te->length)
		bp->inside = TRUE;
}

/**
 * mnt_in_container() - Tell whether a block of the unit lies in a virtual drive.
 * @md:    the unit being scanned
 * @block: the block, counted from the start of the unit
 *
 * mnt_scan_rdb() asks this of an RDSK found past block 0: inside a virtual drive it
 * is the virtual device's RDB, not the unit's. The partition table is read anew on
 * every call, taking up to three scratch sectors on top of the caller's.
 *
 * Return: TRUE if a virtual drive in the table at block 0 covers @block; FALSE if
 * none does, there is no table or block 0 cannot be read
 */
BOOL mnt_in_container(struct MountData *md, ULONG block)
{
	struct BlockProbe bp = { block, FALSE };
	int vbr;
	walk_table(md, probe_container, &bp, &vbr);
	return bp.inside;
}
