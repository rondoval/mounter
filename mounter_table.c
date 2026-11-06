// SPDX-License-Identifier: BSD-2-Clause
// mounter_table.c — the partition table: an MBR with its EBR chains, a GPT, or a
// superfloppy (a filesystem straight at block 0), read once per unit into
// md->table. The scanners and the virtual-drive scan all work from that copy.
//
// Part of the generic autoboot/automount mounter; see LICENSE for the
// copyright and licence covering this file.
#include "mounter_internal.h"
#include "legacy.h"

#define MAX_EXTENDED_PARTITIONS 16

// MBR type of a virtual drive: a whole RDB disk inside the partition.
#define MBR_TYPE_VIRTUAL_DRIVE 0x76
// MBR type of a GPT's protective (or hybrid) entry.
#define MBR_TYPE_GPT_PROTECTIVE 0xee

// Emu68's GPT type of a virtual drive, 3F82EEBC-87C9-4097-8165-89D6540557C0.
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
 * has_boot_signature() - Tell whether a block ends in the 0x55AA signature.
 * @b: the block, at least 512 bytes
 *
 * An MBR, an EBR and a filesystem's boot sector all carry it at byte 510.
 *
 * Return: TRUE if bytes 510 and 511 are 0x55 and 0xAA; FALSE otherwise
 */
static BOOL has_boot_signature(const UBYTE *b)
{
	return b[510] == 0x55 && b[511] == 0xaa;
}

/**
 * mnt_vbr_kind() - Tell which filesystem a Volume Boot Record belongs to.
 * @b:         the block, in a buffer of at least 512 bytes (bytes 0..511 are read)
 * @blocksize: the medium's block size in bytes; a FAT BPB's bytes per sector must
 *             equal it
 * @kind:      out: MOUNTFS_FAT, MOUNTFS_NTFS or MOUNTFS_EXFAT; written only when
 *             the call returns TRUE
 *
 * A VBR ends in 0x55AA just like an MBR, so the BPB fields must be validated to
 * tell a superfloppy (filesystem at block 0) from a partition table. NTFS is known
 * by its OEM ID alone.
 *
 * Return: TRUE for a VBR of one of those families; FALSE for anything else, an
 * MBR included
 */
BOOL mnt_vbr_kind(const UBYTE *b, ULONG blocksize, enum MountFSKind *kind)
{
	if (!has_boot_signature(b))
		return FALSE;
	if (memcmp(b + 3, "NTFS    ", 8) == 0) {
		*kind = MOUNTFS_NTFS;
		return TRUE;
	}
	if (memcmp(b + 3, "EXFAT   ", 8) == 0) {
		// exFAT keeps its geometry outside the FAT BPB, so validate the
		// fields the spec pins down instead: the MustBeZero region (the
		// bytes a FAT/NTFS BPB would fill), and the two shifts/counts the
		// filesystem itself refuses to mount without.
		for (int i = 11; i < 64; i++)
			if (b[i])
				return FALSE;
		if (b[108] < 9 || b[108] > 12)          /* BytesPerSectorShift */
			return FALSE;
		if (b[110] != 1 && b[110] != 2)         /* NumberOfFats */
			return FALSE;
		*kind = MOUNTFS_EXFAT;
		return TRUE;
	}
	if (b[0] != 0xeb && b[0] != 0xe9)          /* x86 jump opcode */
		return FALSE;
	ULONG bps = (ULONG)(b[11] | (b[12] << 8));  /* BPB fields are little endian */
	UBYTE spc = b[13];
	if (bps != blocksize)
		return FALSE;
	if (spc == 0 || (spc & (spc - 1)) != 0)    /* sectors/cluster: power of two */
		return FALSE;
	if (b[16] != 1 && b[16] != 2)              /* number of FATs */
		return FALSE;
	*kind = MOUNTFS_FAT;
	return TRUE;
}

/**
 * add_entry() - Append one partition to the unit's table.
 * @pt:      the table
 * @kind:    what it holds
 * @mbrType: the MBR/EBR type byte; 0 for GPT and superfloppy
 * @number:  its number in the log
 * @start:   first block
 * @length:  in blocks, at least 1
 *
 * A table already holding TABLE_MAX entries keeps them; the rest are logged and
 * dropped.
 */
static void add_entry(struct PartTable *pt, enum PartKind kind, UBYTE mbrType, UBYTE number,
                      uint64_t start, uint64_t length)
{
	if (pt->count >= TABLE_MAX) {
		mnt_log("Partition table: more than %ld entries, the rest ignored\n", (LONG)TABLE_MAX);
		return;
	}
	struct Partition *p = &pt->part[pt->count++];
	p->start = start;
	p->length = length;
	p->kind = kind;
	p->mbrType = mbrType;
	p->number = number;
}

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
 * print_guid() - Log a GUID in its canonical 8-4-4-4-12 text form.
 * @x: the GUID, in on-disk byte order (the first three fields little-endian)
 *
 * Without MOUNTER_LOG the mnt_log() compiles away.
 */
static void print_guid(const GUID *x)
{
	(void)x; // In case we turned debugging off.

	// Somebody has got to be proud of this mixed endian prank.

	mnt_log("%08lx-%04lx-%04lx-%02lx%02lx-%02lx%02lx%02lx%02lx%02lx%02lx",
			(ULONG)__bswap32(x->u.UUID.time_low), (ULONG)__bswap16(x->u.UUID.time_mid),
			(ULONG)__bswap16(x->u.UUID.time_high_and_version),
			(ULONG)x->u.UUID.clock_seq_high_and_reserved, (ULONG)x->u.UUID.clock_seq_low,
			(ULONG)x->u.UUID.node[0], (ULONG)x->u.UUID.node[1], (ULONG)x->u.UUID.node[2],
			(ULONG)x->u.UUID.node[3], (ULONG)x->u.UUID.node[4], (ULONG)x->u.UUID.node[5]);
}

/**
 * mbr_slot_used() - Tell whether an MBR or EBR slot describes a partition.
 * @p: the slot
 *
 * Return: TRUE if its type, f_lba and num_sect are all nonzero; FALSE for an
 * unused slot
 */
static BOOL mbr_slot_used(const struct mbr_partition *p)
{
	return p->type != 0 && p->f_lba != 0 && p->num_sect != 0;
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
 * add_mbr_entry() - Append a used MBR or EBR slot to the table.
 * @pt:     the table
 * @p:      the slot, not extended
 * @base:   the block its f_lba counts from: 0 for a primary slot, its own EBR for
 *          a logical one
 * @number: its number in the log
 *
 * Type 0x76 is a virtual drive, a GPT protective entry is not ours, and any other
 * type may hold data: sticks are often mislabeled, and 0x07 is NTFS or exFAT, so
 * the boot sector decides.
 */
static void add_mbr_entry(struct PartTable *pt, const struct mbr_partition *p, ULONG base,
                          UBYTE number)
{
	enum PartKind kind = p->type == MBR_TYPE_VIRTUAL_DRIVE ? PART_VIRTUAL :
	             p->type == MBR_TYPE_GPT_PROTECTIVE ? PART_OTHER : PART_DATA;
	add_entry(pt, kind, p->type, number, (uint64_t)base + __bswap32(p->f_lba),
	          __bswap32(p->num_sect));
}

/**
 * read_ebr() - Add the logical partitions of one extended partition.
 * @md:     the unit being scanned
 * @base:   the extended partition's first block (its MBR slot's f_lba), the first
 *          EBR
 * @number: in/out: the log number of the next logical partition
 *
 * Walks the EBR chain. Logical partition offsets are relative to their own EBR;
 * the next-EBR link is relative to the extended container base. The walk ends at
 * an unreadable EBR, one without the 0x55AA signature, a link slot that is unused
 * or not is_extended_type(), or after MAX_EXTENDED_PARTITIONS EBRs, which also
 * bounds a chain that loops. Without a free scratch sector nothing is walked.
 */
static void read_ebr(struct MountData *md, ULONG base, UBYTE *number)
{
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return;
	const struct mbr *ebr = (const struct mbr *)buf;
	ULONG block = base;
	int n;

	for (n = 0; n < MAX_EXTENDED_PARTITIONS; n++) {
		if (!mnt_read_block(buf, block, md) || !has_boot_signature(buf))
			break;
		// Slot 0 is the logical partition, slot 1 links to the next EBR.
		const struct mbr_partition *logical = &ebr->part[0];
		const struct mbr_partition *link    = &ebr->part[1];
		if (mbr_slot_used(logical))
			add_mbr_entry(&md->table, logical, block, (*number)++);
		if (!is_extended_type(link->type) || !mbr_slot_used(link))
			break;
		block = base + __bswap32(link->f_lba);
	}
	if (n == MAX_EXTENDED_PARTITIONS) {
		mnt_log("Warning: Extended partition limit (%ld) reached\n",
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
		if (mbr->part[i].type == MBR_TYPE_GPT_PROTECTIVE)
			return TRUE;
	}
	return FALSE;
}

/**
 * is_sane_mbr() - Check that block 0 holds a believable MBR partition table.
 * @mbr:   block 0, its 0x55AA signature already checked
 * @total: the unit's size in blocks, which every used slot must start below; 0 if
 *         unknown, for no bound
 *
 * Rejects boot-code garbage that happens to end in 0x55AA (a filesystem VBR does,
 * too): every slot must look like a real entry and at least one must be used.
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
		if (!mbr_slot_used(p))
			continue;
		if (total && __bswap32(p->f_lba) >= total)
			return FALSE;
		used++;
	}
	return used > 0;
}

/**
 * read_mbr() - Add the partitions of an MBR and its EBR chains.
 * @md:  the unit being scanned
 * @mbr: block 0, already passed by is_sane_mbr(); read in place
 *
 * Primary slots are numbered 1 to 4 by position, logical partitions from 5 on.
 * An extended slot goes to read_ebr(); every other used slot is an entry.
 */
static void read_mbr(struct MountData *md, const struct mbr *mbr)
{
	UBYTE logical = 5;

	for (int i = 0; i < 4; i++) {
		const struct mbr_partition *p = &mbr->part[i];
		if (!mbr_slot_used(p))
			continue;
		if (is_extended_type(p->type))
			read_ebr(md, __bswap32(p->f_lba), &logical);
		else
			add_mbr_entry(&md->table, p, 0, (UBYTE)(i + 1));
	}
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
 * @gpt:       the header, in a sector of @blocksize bytes; writable, as
 *             header_crc32 is zeroed for the CRC and then restored
 * @blocksize: the block size in bytes, which bounds the header size
 *
 * UEFI-spec checks: signature "EFI PART", my_lba 1 (the header must claim to live
 * at the LBA it was read from), size from sizeof(struct gpt) up to the block size,
 * and the self-CRC32. This is what tells a live GPT from a stale header left at
 * block 1 by an earlier repartitioning. The entry-array CRC is skipped; read_gpt()
 * bounds-checks what it reads.
 *
 * Return: TRUE if every check passes, FALSE otherwise
 */
static BOOL is_valid_gpt_header(struct gpt *gpt, ULONG blocksize)
{
	if (memcmp(gpt->signature, "EFI PART", 8) != 0)
		return FALSE;
	if (__bswap64(gpt->my_lba) != 1)
		return FALSE;
	ULONG size = __bswap32(gpt->size);
	if (size < sizeof(struct gpt) || size > blocksize)
		return FALSE;
	ULONG stored = gpt->header_crc32;
	gpt->header_crc32 = 0;   /* the CRC is computed with its own field zeroed */
	ULONG computed = crc32((const UBYTE *)gpt, size);
	gpt->header_crc32 = stored;
	return computed == __bswap32(stored);
}

/**
 * read_gpt() - Add every used entry of a GPT's entry array.
 * @md:  the unit being scanned
 * @gpt: the GPT header, validated by is_valid_gpt_header()
 *
 * Reads number_of_entries entries (at most TABLE_MAX) of size_of_entry bytes from
 * entries_lba on, a 64-bit block number. An entry with first_lba and last_lba both
 * 0 is unused; one whose last_lba is below its first_lba is skipped. Basic Data
 * entries may hold data, Emu68's type is a virtual drive, any other type is not
 * ours. An unreadable block of the array ends the walk.
 *
 * Return: FALSE if size_of_entry is not 128 x 2^n bytes (UEFI) up to the block
 * size, or no scratch sector is free; TRUE otherwise, an empty table or a walk cut
 * short by an unreadable block included
 */
static BOOL read_gpt(struct MountData *md, const struct gpt *gpt)
{
	uint64_t block = __bswap64(gpt->entries_lba);
	ULONG count = __bswap32(gpt->number_of_entries);
	const ULONG size = __bswap32(gpt->size_of_entry);

	if (size < sizeof(struct gpt_partition) || size > md->blocksize || (size & (size - 1)))
		return FALSE;
	if (count > TABLE_MAX)   /* standard table size; don't chase garbage counts */
		count = TABLE_MAX;
	const ULONG perBlock = md->blocksize / size;

	// The entry array gets its own sector: the header and block 0 stay intact.
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return FALSE;

	for (ULONG i = 0, pos = 0; i < count; i++) {
		if (pos == 0 && !mnt_read_block(buf, block++, md))
			break;
		const struct gpt_partition *e = (const struct gpt_partition *)(buf + pos * size);
		if (++pos == perBlock)
			pos = 0;

		if (e->first_lba == 0 && e->last_lba == 0)
			continue;
		const uint64_t first = __bswap64(e->first_lba);
		const uint64_t last = __bswap64(e->last_lba);
		if (last < first) {
			mnt_log("GPT entry %lu ends before it starts\n", i + 1);
			continue;
		}
		enum PartKind kind = guid_equal(&e->partition_type, &GUID_VIRTUAL_DRIVE) ? PART_VIRTUAL :
		             guid_equal(&e->partition_type, &GUID_BASIC_DATA) ? PART_DATA : PART_OTHER;
		if (kind == PART_OTHER) {
			mnt_log("GPT entry %lu: type ", i + 1);
			print_guid(&e->partition_type);
			mnt_log("\n");
		}
		add_entry(&md->table, kind, 0, (UBYTE)(i + 1), first, last - first + 1);
	}

	mnt_sector_drop(md, buf);
	return TRUE;
}

/**
 * read_block0() - Tell how block 0 is laid out and add what its table holds.
 * @md:     the unit being scanned; md->table is empty on entry
 * @block0: block 0, read; left intact
 *
 * A GPT (gated by its protective or hybrid MBR entry, so a stale GPT header on a
 * since-repartitioned disk can't override the current table), else a filesystem
 * at block 0 (a superfloppy: one PART_DATA entry covering the medium, when its
 * size is known), else a sane MBR with its EBR chains. A filesystem VBR ends in
 * 0x55AA too, which is why it is told apart before the MBR is believed. A valid
 * GPT header commits the unit to GPT even when its entry array is refused.
 */
static void read_block0(struct MountData *md, const UBYTE *block0)
{
	struct PartTable *pt = &md->table;
	const struct mbr *mbr = (const struct mbr *)block0;
	BOOL signature = has_boot_signature(block0);

	if (signature && has_protective_entry(mbr)) {
		// The GPT header gets its own sector so block 0 stays intact for the
		// MBR fallback if the header is invalid.
		UBYTE *hdr = mnt_sector_take(md);
		if (hdr) {
			BOOL valid = mnt_read_block(hdr, 1, md) &&
			             is_valid_gpt_header((struct gpt *)hdr, md->blocksize);
			if (valid && read_gpt(md, (const struct gpt *)hdr))
				pt->scheme = TABLE_GPT;
			mnt_sector_drop(md, hdr);
			if (valid)
				return;
		}
		// Fall through: a hybrid MBR may still carry entries; a pure
		// protective one fails is_sane_mbr or yields only its 0xEE entry.
		mnt_log("GPT protective entry but no valid GPT header at block 1\n");
	}

	enum MountFSKind kind;
	if (mnt_vbr_kind(block0, md->blocksize, &kind)) {
		if (md->totalsectors) {
			pt->scheme = TABLE_SUPERFLOPPY;
			add_entry(pt, PART_DATA, 0, 1, 0, md->totalsectors);
		}
		return;
	}
	if (signature && is_sane_mbr(mbr, md->totalsectors)) {
		pt->scheme = TABLE_MBR;
		read_mbr(md, mbr);
	}
}

/**
 * dump_table() - Log a unit's partition table.
 * @pt: the table
 *
 * One line per entry: its number, kind, MBR type, first block and length.
 * Without MOUNTER_LOG this is empty.
 */
static void dump_table(const struct PartTable *pt)
{
#if defined(MOUNTER_LOG)
	static const char *const schemes[] = { "none", "MBR", "GPT", "superfloppy" };
	static const char *const kinds[] = { "other", "data", "virtual" };

	mnt_log("Partition table: %s\n", schemes[pt->scheme]);
	for (UWORD i = 0; i < pt->count; i++) {
		const struct Partition *p = &pt->part[i];
		mnt_log(" %3ld %-7s type %02lx start %08lx%08lx length %08lx%08lx\n",
		       (LONG)p->number, kinds[p->kind], (ULONG)p->mbrType,
		       (ULONG)(p->start >> 32), (ULONG)p->start,
		       (ULONG)(p->length >> 32), (ULONG)p->length);
	}
#else
	(void)pt;
#endif
}

/**
 * mnt_read_table() - Read the unit's partition table into md->table.
 * @md: the unit: md->SysBase, md->request, md->blocksize, md->totalsectors set
 *
 * md->table is emptied first, then filled by read_block0(). A block under 512
 * bytes holds no MBR, GPT or VBR, so such a unit has no table. Every scratch
 * sector taken is released before the call returns: up to three while it runs
 * (block 0, the GPT header, the entry array).
 *
 * Return: TRUE if block 0 was read, or the blocks are too small to hold a table;
 * FALSE if block 0 cannot be read or no scratch sector is free. md->table's
 * scheme is TABLE_NONE when there is no table, either way
 */
BOOL mnt_read_table(struct MountData *md)
{
	struct PartTable *pt = &md->table;

	pt->scheme = TABLE_NONE;
	pt->count = 0;
	if (md->blocksize < 512)
		return TRUE;
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return FALSE;
	BOOL readable = mnt_read_block(buf, 0, md);
	if (readable)
		read_block0(md, buf);
	mnt_sector_drop(md, buf);
	dump_table(pt);
	return readable;
}

/**
 * mnt_in_virtual_drive() - Tell whether a block lies in one of the table's
 * virtual drives.
 * @pt:    the unit's table, from mnt_read_table()
 * @block: the block, counted from the start of the unit
 *
 * mnt_scan_rdb() asks this of an RDSK found past block 0: inside a virtual drive
 * it is the virtual device's RDB, not the unit's.
 *
 * Return: TRUE if a PART_VIRTUAL entry covers @block; FALSE otherwise
 */
BOOL mnt_in_virtual_drive(const struct PartTable *pt, ULONG block)
{
	for (UWORD i = 0; i < pt->count; i++) {
		const struct Partition *p = &pt->part[i];
		if (p->kind == PART_VIRTUAL && block >= p->start && block - p->start < p->length)
			return TRUE;
	}
	return FALSE;
}
