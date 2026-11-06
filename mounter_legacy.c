// SPDX-License-Identifier: BSD-2-Clause
// mounter_legacy.c — non-RDB media: mounts the FAT, NTFS and exFAT volumes of
// the unit's partition table (mounter_table.c), a superfloppy included.
//
// Part of the generic autoboot/automount mounter; see LICENSE for the
// copyright and licence covering this file.
#include "mounter_internal.h"

/**
 * mount_partition() - Mount one PART_DATA entry of the partition table.
 * @md: the unit being scanned
 * @p:  the entry
 *
 * The partition's own first block picks the recipe: mnt_vbr_kind() names the
 * family and md->fs[] its MountFS. The MBR type byte or GPT type GUID only got
 * the entry here: 0x07 is NTFS or exFAT, and sticks are often mislabeled. The
 * blocks map to de_LowCyl..de_HighCyl at one block per cylinder, which takes
 * ULONG blocks, so a partition reaching 2^32 blocks is skipped. So is one whose
 * first block cannot be read or holds no filesystem the mounter knows, or whose
 * family has no recipe.
 *
 * Nothing this path mounts can ever be booted from, so every node it creates goes
 * in at -128 ("don't even bother to boot from this device", per expansion.doc).
 * mnt_vbr_kind() only ever answers FAT, NTFS or exFAT, and none of those carries
 * an Amiga boot block — their VBR occupies the very sector strap would read one
 * from.
 *
 * Return: TRUE if the volume is mounted now or was already (MOUNT_OK,
 * MOUNT_ALREADY); FALSE if it was skipped, deferred or failed
 */
static BOOL mount_partition(struct MountData *md, const struct Partition *p)
{
	if (((p->start + p->length - 1) >> 32) != 0) {
		mnt_log("Skipping partition %ld beyond 2^32 blocks\n", (LONG)p->number);
		return FALSE;
	}
	ULONG first = (ULONG)p->start;

	// Sniff the boot sector in a sector of its own.
	UBYTE *vbr = mnt_sector_take(md);
	if (!vbr)
		return FALSE;
	enum MountFSKind kind;
	BOOL known = mnt_read_block(vbr, first, md) && mnt_vbr_kind(vbr, md->blocksize, &kind);
	mnt_sector_drop(md, vbr);
	const struct MountFS *fs = known ? md->fs[kind] : NULL;
	if (!fs) {
		mnt_log("Skipping partition %ld at %lu: unsupported filesystem\n", (LONG)p->number, first);
		return FALSE;
	}

	struct Volume vol = {
		.fs         = fs,
		.firstBlock = first,
		.lastBlock  = (ULONG)(p->start + p->length - 1),
		.nameHint   = fs->dosName ? fs->dosName : (const UBYTE *)"MS0",
		.bootPri    = MOUNT_NEVER_BOOT,
	};
	enum MountOutcome outcome = mnt_mount_volume(md, &vol);
	return outcome == MOUNT_OK || outcome == MOUNT_ALREADY;
}

/**
 * mnt_scan_legacy() - Mount a non-RDB medium's FAT, NTFS and exFAT volumes.
 * @md: the unit being scanned, md->table read by probe_unit()
 *
 * Mounts every PART_DATA entry of md->table in table order; a superfloppy is the
 * table's one entry. With MSF_LEGACY_FIRST_ONLY the first volume that mounts, or
 * was already mounted, ends the scan. The count is in md->mounted.
 *
 * Return: TRUE if the medium was recognized (md->table has a scheme), whether or
 * not anything mounted; FALSE otherwise
 */
BOOL mnt_scan_legacy(struct MountData *md)
{
	const struct PartTable *pt = &md->table;

	if (pt->scheme == TABLE_NONE)
		return FALSE;
	for (UWORD i = 0; i < pt->count; i++) {
		if (pt->part[i].kind != PART_DATA)
			continue;
		if (mount_partition(md, &pt->part[i]) && (md->flags & MSF_LEGACY_FIRST_ONLY))
			break;
	}
	return TRUE;
}
