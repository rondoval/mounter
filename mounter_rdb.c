
// mounter_rdb.c — RigidDiskBlock: the RDSK search, the PART chain, and the FSHD
// chain that names (and, through mounter_hunk.c, loads) each partition's
// filesystem.
//
// Part of the generic autoboot/automount mounter; see mounter.c for the
// copyright and licence covering this file.
#include "mounter_internal.h"

// Hardening caps against corrupt/hostile on-disk metadata. The RDB PART and FSHD
// chains are singly linked by block number; a cyclic link would otherwise loop
// forever (the PART loop re-mounting each lap).
#define MAX_RDB_PARTITIONS 128          // realistic ceiling; > any sane RDB
#define MAX_RDB_FILESYS    64           // FSHD chain length

/**
 * parse_fshd() - Find a partition's filesystem; load it off the disk if needed.
 * @block:   first FSHD block (rdb_FileSysHeaderList); RDB_END = none
 * @dostype: the partition's de_DosType
 * @md:      mount state; two scratch sectors are taken from its pool and dropped
 *           again
 *
 * Walks the fhb_Next chain, at most MAX_RDB_FILESYS blocks, to the first FSHD
 * for @dostype; an unreadable or invalid block ends the walk. Unless
 * mnt_fse_new() finds FileSystem.resource's entry good enough, the FSHD's LSEG
 * chain is loaded (mnt_load_lseg()) and the entry registered
 * (mnt_fse_register()); an entry that is not added takes its code with it
 * (mnt_unload_lseg()). If either scratch sector cannot be had, only the
 * FileSystem.resource lookup runs.
 *
 * Return: the FileSysEntry to mount with: the one just loaded and registered,
 * else FileSystem.resource's entry for @dostype (also when no FSHD on this disk
 * carries the dostype or loading it failed); NULL if there is neither
 */
static struct FileSysEntry *parse_fshd(ULONG block, ULONG dostype, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct FileSysEntry *fse = NULL;
	// The FileSysHeaderBlock and its LoadSegBlock chain live in two scratch
	// sectors that must coexist; both are released before returning.
	UBYTE *buf = mnt_sector_take(md);
	UBYTE *segbuf = mnt_sector_take(md);

	if (buf && segbuf) {
		const struct FileSysHeaderBlock *fshb = (const struct FileSysHeaderBlock *)buf;
		for (int i = 0; i < MAX_RDB_FILESYS && block != RDB_END; i++) {
			if (!mnt_read_rdb_block(buf, block, IDNAME_FILESYSHEADER, md))
				break;
			dbg("FSHD found, block %lu, dostype %08lx, looking for dostype %08lx\n", block, fshb->fhb_DosType, dostype);
			if (fshb->fhb_DosType == dostype) {
				fse = mnt_fse_new(md, fshb);
				if (fse) {
					BPTR seglist = mnt_load_lseg(md, (ULONG)fshb->fhb_SegListBlocks, segbuf);
					fse->fse_SegList = seglist;
					// Not added (load failed, or another caller added it first):
					// the entry is freed, the code is ours to free.
					if (!mnt_fse_register(md, fse)) {
						mnt_unload_lseg(md, seglist);
						fse = NULL;
					}
				}
				break;
			}
			block = fshb->fhb_Next;
		}
	}
	// No FSHD on this disk carried the dostype (or loading it failed): fall back to
	// whatever FileSystem.resource already has for it, which may be nothing.
	if (!fse) {
		fse = mnt_find_filesystem(dostype, 0, SysBase);
	}
	if (segbuf) mnt_sector_drop(md, segbuf);
	if (buf)    mnt_sector_drop(md, buf);
	return fse;
}

/**
 * parse_part() - Parse one PART block and mount what it describes.
 * @buf:          the caller's scratch sector; overwritten with the PART block,
 *                whose pb_DriveName is clamped in place
 * @block:        the PART block number
 * @filesysblock: first FSHD block (rdb_FileSysHeaderList), for parse_fshd()
 * @md:           mount state; MSF_NO_BOOT in md->flags makes the volume
 *                non-bootable
 *
 * A PBFF_NOMOUNT partition is skipped. What was mounted lands in md->mounted,
 * counted by mnt_mount_volume().
 *
 * Return: the next PART block in the chain (pb_Next), also when this partition
 * is skipped, the Volume cannot be allocated or the mount fails; RDB_END when
 * @block cannot be read as a valid PART block
 */
static ULONG parse_part(UBYTE *buf, ULONG block, ULONG filesysblock, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct PartitionBlock *part = (struct PartitionBlock*)buf;

	if (!mnt_read_rdb_block(buf, block, IDNAME_PARTITION, md)) {
		return RDB_END;
	}
	dbg("PART found, block %lu\n", block);
	ULONG nextpartblock = part->pb_Next;
	if (part->pb_Flags & PBFF_NOMOUNT) {
		return nextpartblock;
	}

	// Heap rather than stack: this is the deepest point of the whole mounter, and
	// a Volume carries a DosEnvec.
	struct Volume *vol = AllocMem(sizeof(struct Volume), MEMF_PUBLIC | MEMF_CLEAR);
	if (!vol) {
		return nextpartblock;
	}

	// pb_Environment[0] (de_TableSize) is untrusted disk data. Clamp it to what
	// vol->pp.de (a struct DosEnvec) holds before copying, or the copy overruns
	// the ParameterPacket — and clamp de_TableSize itself so MakeDosNode, which
	// reads (de_TableSize + 1) longs, stays in bounds.
	ULONG tablesize = part->pb_Environment[0];
	if (tablesize > sizeof(struct DosEnvec) / sizeof(ULONG) - 1)
		tablesize = sizeof(struct DosEnvec) / sizeof(ULONG) - 1;
	CopyMem(&part->pb_Environment, &vol->pp.de, (tablesize + 1) * sizeof(ULONG));
	vol->pp.de.de_TableSize = tablesize;

	// The filesystem comes off this disk (or FileSystem.resource); no recipe is
	// involved, so vol->fs stays NULL and mnt_mount_volume() uses vol->fse.
	vol->fse = parse_fshd(filesysblock, vol->pp.de.de_DosType, md);

	// The RDB length byte is untrusted disk data. Clamp both it and the content
	// to what the field holds (length byte + chars + NUL) so every downstream
	// BSTR/C-string view of the name agrees on the length.
	UBYTE len = *part->pb_DriveName;
	if (len > (int)sizeof(part->pb_DriveName) - 2)
		len = (int)sizeof(part->pb_DriveName) - 2;
	part->pb_DriveName[0] = len;
	part->pb_DriveName[len + 1] = 0;
	vol->nameHint = part->pb_DriveName + 1;
	dbg("PART '%s'\n", vol->nameHint);

	// Only an RDB partition can actually be booted from, and only if its own RDB
	// says so. Note de_BootPri keeps whatever the disk said either way — the early
	// boot menu reads that, while strap orders by what we pass to AddBootNode().
	vol->bootPri = (part->pb_Flags & PBFF_BOOTABLE) ? vol->pp.de.de_BootPri : MOUNT_NEVER_BOOT;
	if (md->flags & MSF_NO_BOOT)
		vol->bootPri = MOUNT_NEVER_BOOT;

	mnt_mount_volume(md, vol);

	FreeMem(vol, sizeof(struct Volume));
	return nextpartblock;
}

/**
 * parse_rdsk() - Walk the PART chain, mounting as we go.
 * @buf: the scratch sector holding the RDSK block; reused for every PART block,
 *       so the RDSK is gone on return
 * @md:  mount state; md->wasLastDev is set from RDBFF_LAST in rdb_Flags
 *
 * What was mounted lands in md->mounted. The walk is capped at
 * MAX_RDB_PARTITIONS blocks, so a cyclic pb_Next cannot re-mount the same
 * partitions forever; parse_part() also ends it on an unreadable block.
 */
static void parse_rdsk(UBYTE *buf, struct MountData *md)
{
	const struct RigidDiskBlock *rdb = (const struct RigidDiskBlock *)buf;
	ULONG partblock = rdb->rdb_PartitionList;
	ULONG filesysblock = rdb->rdb_FileSysHeaderList;

	md->wasLastDev = (rdb->rdb_Flags & RDBFF_LAST) != 0;
	for (int i = 0; i < MAX_RDB_PARTITIONS && partblock != RDB_END; i++)
		partblock = parse_part(buf, partblock, filesysblock, md);
}

/**
 * mnt_scan_rdb() - Search for an RDB and, if there is one, mount its partitions.
 * @md: mount state of the open unit, md->table read for a direct-access unit; one
 *      scratch sector is taken from its pool and dropped again
 *
 * Reads blocks 0 to RDB_LOCATION_LIMIT - 1, passing over unreadable ones, and
 * takes the first valid RDSK block: rdb_ID IDNAME_RIGIDDISK and a good checksum,
 * as for every other RDB block. An RDSK past block 0 that lies in one of
 * md->table's virtual drives is the virtual device's RDB, and skipped.
 *
 * Return: TRUE if this medium was recognized as RDB, even if nothing on it
 * mounted; the count is in md->mounted. FALSE if no RDSK was found or no
 * scratch sector was available
 */
BOOL mnt_scan_rdb(struct MountData *md)
{
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return FALSE;
	BOOL found = FALSE;
	for (UWORD i = 0; i < RDB_LOCATION_LIMIT; i++) {
		if (!mnt_read_rdb_block(buf, i, IDNAME_RIGIDDISK, md))
			continue;
		if (i && mnt_in_virtual_drive(&md->table, i)) {
			dbg("RDB at block %lu is a virtual drive's\n", (ULONG)i);
			continue;
		}
		dbg("RDB found, block %lu\n", (ULONG)i);
		parse_rdsk(buf, md);
		found = TRUE;
		break;
	}
	mnt_sector_drop(md, buf);
	return found;
}
