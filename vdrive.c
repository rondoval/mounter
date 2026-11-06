/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * vdrive.c — virtual drives as units of a device of their own: the host's
 * lifecycle calls. A rescan of a parent unit fences its virtual drives and brings
 * their units online, a parent that is gone takes them offline, and the device
 * goes when the host expunges. vdrive.h is the host's side; vdrive_internal.h
 * maps the other units.
 *
 * Part of the generic autoboot/automount mounter; see LICENSE for the
 * copyright and licence covering this file.
 */
#include "vdrive_internal.h"

/**
 * block_shift() - Turn a block size into a shift count.
 * @size: the block size in bytes
 *
 * A power of two's trailing zeros are its log2: __builtin_ctzl(), bfffo from
 * the 68020 on and libgcc's __ctzsi2() on a 68000.
 *
 * Return: log2(@size) when @size is a power of two from 2 to 2^31; 0 for 1 and
 * for anything else
 */
static UBYTE block_shift(ULONG size)
{
	if (size < 2 || (size & (size - 1)))       /* not a power of two above 1 */
		return 0;
	return (UBYTE)__builtin_ctzl(size);
}

/**
 * unit_changed() - Count a medium change on a unit and announce it.
 * @device: the device base (for SysBase)
 * @unit:   the unit; caller holds Forbid() (its changeInts list)
 *
 * Bumps the change count, then Cause()s the struct Interrupt in io_Data of each
 * held TD_ADDCHANGEINT request; one with NULL io_Data is skipped. The requests
 * stay held.
 */
static void unit_changed(struct VDrive *device, struct VDUnit *unit)
{
	struct ExecBase *SysBase = device->SysBase;
	unit->changeCount++;
	for (struct MinNode *node = unit->changeInts.mlh_Head; node->mln_Succ; node = node->mln_Succ)
	{
		struct IOStdReq *changeInt = (struct IOStdReq *)node;
		if (changeInt->io_Data)
			Cause((struct Interrupt *)changeInt->io_Data);
	}
}

/**
 * parent_fenced() - Record whether any of a parent's units is fenced.
 * @device: the device base
 * @parent: the parent record; caller holds Forbid()
 * @fenced: whether one is now
 *
 * Keeps device->fencedParents, the parents with a unit fenced, in step.
 */
static void parent_fenced(struct VDrive *device, struct VDriveParent *parent, BOOL fenced)
{
	if (!parent->fenced && fenced)
		device->fencedParents++;
	else if (parent->fenced && !fenced)
		device->fencedParents--;
	parent->fenced = fenced;
}

/**
 * parent_offline() - Take a parent unit's virtual drives offline.
 * @parent:  the unit's record; NULL accepted (nothing to do)
 * @unfence: TRUE to lift its fence as well
 *
 * Under Forbid(), from any task and with no I/O: the units that are online go
 * offline, their change counts bumped and their change interrupts Cause()d.
 * The units and the held parent open stay, for a rescan to bring back.
 */
static void parent_offline(struct VDriveParent *parent, BOOL unfence)
{
	if (!parent)
		return;
	struct VDrive *device = parent->device;
	struct ExecBase *SysBase = device->SysBase;
	Forbid();
	for (ULONG slot = 0; slot < VDRIVE_SLOTS; slot++)
	{
		struct VDUnit *unit = parent->units[slot];
		if (!unit)
			continue;
		if (unfence)
			unit->fenced = FALSE;
		if (unit->online)
		{
			unit->online = FALSE;
			unit_changed(device, unit);
		}
	}
	if (unfence)
		parent_fenced(device, parent, FALSE);
	Permit();
}

/**
 * parent_update() - Bring a parent's units online with its virtual drives.
 * @device:    the device base
 * @parent:    the parent record, with a unit (or NULL where vd_unit_new()
 *             failed) in each slot below the lesser of @count and VDRIVE_SLOTS
 * @drives:    the first virtual drives, as many as VDRIVE_SLOTS, in table order
 * @count:     how many virtual drives the table has, at least 1
 * @shift:     log2 of the parent's block size
 * @removable: the parent's medium is
 *
 * Under Forbid(). A slot's unit comes online with its drive, and the drive's
 * blocks turn read-only on the parent (fenced), unless the drive is 2^32 blocks
 * or more (dg_TotalSectors cannot say so). A drive with no unit online, past
 * VDRIVE_SLOTS, too large, or its unit not allocated, stays an ordinary
 * partition of the parent: reachable one way only, it needs no fence. A slot
 * without a drive any longer goes offline and unfenced. Every unit of the
 * parent has its change count bumped and its change interrupts Cause()d.
 *
 * Return: one more than the highest slot that came online; 0 if none did
 */
static ULONG parent_update(struct VDrive *device, struct VDriveParent *parent,
                           const struct Partition *drives, ULONG count, UBYTE shift, BOOL removable)
{
	struct ExecBase *SysBase = device->SysBase;
	ULONG shown = count < VDRIVE_SLOTS ? count : VDRIVE_SLOTS;
	ULONG online = 0;

	Forbid();
	parent->blockShift = shift;
	parent->removable = removable;
	for (ULONG slot = 0; slot < VDRIVE_SLOTS; slot++)
	{
		struct VDUnit *unit = parent->units[slot];
		if (!unit)
			continue;
		unit->online = slot < shown && (drives[slot].length >> 32) == 0;
		unit->fenced = unit->online;
		if (unit->online)
		{
			unit->start = drives[slot].start;
			unit->length = (ULONG)drives[slot].length;
			online = slot + 1;
		}
		unit_changed(device, unit);
	}
	parent_fenced(device, parent, online != 0);     /* a unit is fenced when it comes online */
	Permit();
	return online;
}

/**
 * VDriveRefresh() - Rescan one parent unit for virtual drives.
 * @devicePtr:  the host's device pointer: NULL until the first virtual drive
 *              anywhere, when the device is created into *@devicePtr (left NULL
 *              if that fails)
 * @config:     the host's configuration, the same on every call: its SysBase
 *              does the scan, and its strings must outlive the device
 * @parentUnit: the host's unit to rescan
 * @parentPtr:  the host's handle for @parentUnit, kept with its own unit: set to
 *              the unit's record whenever it has one, which it keeps once it has
 *              had a virtual drive; NULL until then, or when the record cannot
 *              be made
 *
 * Rescan one parent unit: its geometry, then the partition table of a
 * direct-access medium (a CD's isohybrid MBR describes no disks), read by the
 * mounter (mnt_find_virtual_drives()) from the calling task. Only a block size
 * that is a power of two from 256 to MAX_BLOCKSIZE bytes is scanned. Task
 * context only, and never the parent's own I/O task (the scan does I/O on the
 * parent).
 *
 * With virtual drives found: creates the device on the first virtual drive
 * anywhere, holds the parent unit open from its first (vd_parent_get()),
 * allocates a unit for each of the first VDRIVE_SLOTS, then brings the units
 * online and fences their drives' blocks (parent_update()); a virtual drive
 * past them stays an ordinary partition of the parent. With none, or on a
 * medium that cannot hold one: as VDriveParentGone(). When the medium cannot
 * be read (memory, OpenDevice(), TD_GETGEOMETRY or block 0 failing) the units
 * go offline the same way, but stay fenced until a scan finds no virtual drive
 * or VDriveParentGone() lifts it.
 *
 * Return: n, the units to mount, VDRIVE_UNIT(@parentUnit, 0..n-1), with
 * MSF_NO_LEGACY | MSF_NO_CD | MSF_IGNORE_LAST: one more than the highest online
 * slot, so a slot below it is offline only for a drive of 2^32 blocks or more
 * or a unit that could not be allocated; 0 when the parent has no virtual drive
 * or its medium cannot be read, and also when the device or the parent record
 * could not be created (out of memory), which leaves the new drives unfenced
 */
ULONG VDriveRefresh(struct VDrive **devicePtr, const struct VDriveConfig *config, ULONG parentUnit,
                    struct VDriveParent **parentPtr)
{
	struct DriveGeometry geometry;
	struct Partition drives[VDRIVE_SLOTS];

	LONG found = mnt_find_virtual_drives(config->SysBase, (const UBYTE *)config->parentName,
	                                     parentUnit, &geometry, drives, VDRIVE_SLOTS);
	/* The units address their parent in shifts: a block size that is not a power
	 * of two cannot have them. */
	UBYTE shift = found > 0 ? block_shift(geometry.dg_SectorSize) : 0;
	if (found > 0 && shift == 0)
		found = 0;
	if (found <= 0)
	{
		/* Unreadable (found < 0): the units cannot be trusted, but nothing says
		 * the blocks stopped being virtual drives, so they stay fenced. */
		struct VDriveParent *parent = vd_parent_lookup(*devicePtr, parentUnit);
		parent_offline(parent, found == 0);
		*parentPtr = parent;
		return 0;
	}

	if (!*devicePtr)
		*devicePtr = vd_create(config);
	struct VDrive *device = *devicePtr;
	struct VDriveParent *parent = device ? vd_parent_get(device, parentUnit) : NULL;
	*parentPtr = parent;
	if (!parent)
		return 0;
	ULONG count = (ULONG)found;
	for (ULONG slot = 0; slot < count && slot < VDRIVE_SLOTS; slot++)
		if (!parent->units[slot])
			parent->units[slot] = vd_unit_new(device, parent);

	BOOL removable = (geometry.dg_Flags & DGF_REMOVABLE) != 0;
	ULONG online = parent_update(device, parent, drives, count, shift, removable);
	mnt_log("%s unit %lu: %lu virtual drive(s), %lu exposed\n", config->parentName, parentUnit,
	       count, online);
	return online;
}

/**
 * VDriveParentGone() - Take a parent unit's virtual drives away.
 * @parent: the handle of the unit whose medium, or which itself, is gone
 *          (VDriveRefresh()); NULL accepted: it has no virtual drives
 *
 * No I/O, from any task (a task: the device's state is guarded by Forbid()):
 * clears the fence, takes the units that are online offline, bumps their
 * change counts and Cause()s their change interrupts, all under Forbid(). The
 * units, the held parent open and the handle stay, for VDriveRefresh() to bring
 * back.
 */
void VDriveParentGone(struct VDriveParent *parent)
{
	parent_offline(parent, TRUE);
}

/**
 * VDriveHeldOpens() - Count the host device opens the virtual device holds.
 * @device: the device; NULL accepted
 *
 * One per parent unit that ever had a virtual drive (vd_parent_get()); none is
 * dropped before VDriveDestroy(). A host is otherwise unused when its open
 * count is down to these (plus its own), and only then destroys the device to
 * expunge.
 *
 * Return: the number of parent units held open; 0 for a NULL @device
 */
ULONG VDriveHeldOpens(struct VDrive *device)
{
	if (!device)
		return 0;
	struct ExecBase *SysBase = device->SysBase;
	ULONG count = 0;
	Forbid();
	for (struct MinNode *node = device->parents.mlh_Head; node->mln_Succ; node = node->mln_Succ)
		count++;
	Permit();
	return count;
}

/**
 * VDriveDestroy() - Remove and free the virtual device.
 * @device: the device; NULL accepted. Invalid after TRUE
 *
 * Refuses, changing nothing, while a unit is open (lib_OpenCnt) or a request is
 * in flight (device->busyWrapped, the CMD_UPDATE of a unit's last close
 * included).
 * Else removes the device from the device list under Forbid(), closes the
 * parent units it holds open and frees the parent records with their units
 * (vd_parents_free()), then the wrapped requests and the device base
 * (vd_free()). Task context.
 *
 * A request in flight that is on the forward port was ReplyMsg()ed by a host
 * reply path that skipped VDriveComplete(): it never comes back, and is logged.
 *
 * Return: TRUE when the device is gone, or @device is NULL; FALSE while a unit
 * is open or a request is in flight
 */
BOOL VDriveDestroy(struct VDrive *device)
{
	if (!device)
		return TRUE;
	struct ExecBase *SysBase = device->SysBase;

	Forbid();
	if (device->lib.lib_OpenCnt || !list_empty(&device->busyWrapped))
	{
		BOOL stranded = !IsMsgPortEmpty(&device->forwardPort);
		Permit();
		if (stranded)
		{
			mnt_log("%s: a request was ReplyMsg()ed past VDriveComplete()\n",
			        device->lib.lib_Node.ln_Name);
		}
		return FALSE;
	}
	Remove(&device->lib.lib_Node);
	Permit();

	vd_parents_free(device);
	vd_free(device);
	return TRUE;
}
