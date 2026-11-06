/*
 * vdrive_unit.c — parent records and the units of their virtual drives: made,
 * looked up, read and freed. What a rescan changes in them is vdrive.c's.
 *
 * Part of the generic autoboot/automount mounter; see mounter.c for the
 * copyright and licence covering this file.
 */
#include "vdrive_internal.h"

/**
 * vd_parent_find() - Look up the record of a parent unit.
 * @device:     the device base; caller holds Forbid() (the parents list)
 * @parentUnit: the parent's unit number on the host device
 *
 * Return: the record; NULL when there is none: @parentUnit never had a virtual
 * drive, or vd_parent_get() failed for it
 */
struct VDriveParent *vd_parent_find(struct VDrive *device, ULONG parentUnit)
{
	for (struct MinNode *node = device->parents.mlh_Head; node->mln_Succ; node = node->mln_Succ)
	{
		struct VDriveParent *parent = (struct VDriveParent *)node;
		if (parent->unitNumber == parentUnit)
			return parent;
	}
	return NULL;
}

/**
 * vd_parent_lookup() - Look up the record of a parent unit, under Forbid().
 * @device:     the device base; NULL accepted
 * @parentUnit: the parent's unit number on the host device
 *
 * Task context.
 *
 * Return: as vd_parent_find(); NULL as well for a NULL @device
 */
struct VDriveParent *vd_parent_lookup(struct VDrive *device, ULONG parentUnit)
{
	if (!device)
		return NULL;
	struct ExecBase *SysBase = device->SysBase;
	Forbid();
	struct VDriveParent *parent = vd_parent_find(device, parentUnit);
	Permit();
	return parent;
}

/**
 * vd_parent_get() - Find the record of a parent unit, creating it on first use.
 * @device:     the device base
 * @parentUnit: the parent's unit number on the host device
 *
 * The parent record, created on first use with the parent opened for good: its
 * io_Device and io_Unit are what every forwarded request is sent to. Only
 * vd_parents_free() closes it, and each one counts in VDriveHeldOpens(). Its
 * mn_ReplyPort is a PA_IGNORE port of its own: it is never sent, and must never
 * pass for a forwarded request, which the forward port marks. Task context:
 * OpenDevice() on the parent.
 *
 * Return: the record; NULL when it is new and AllocMem() or OpenDevice() fails
 */
struct VDriveParent *vd_parent_get(struct VDrive *device, ULONG parentUnit)
{
	struct ExecBase *SysBase = device->SysBase;

	struct VDriveParent *parent = vd_parent_lookup(device, parentUnit);
	if (parent)
		return parent;
	parent = AllocMem(sizeof(struct VDriveParent), MEMF_PUBLIC | MEMF_CLEAR);
	if (!parent)
		return NULL;
	parent->device = device;
	parent->unitNumber = parentUnit;
	parent->openReq.io_Message.mn_ReplyPort = &device->openPort;
	parent->openReq.io_Message.mn_Length = sizeof(struct IOStdReq);
	if (OpenDevice((CONST_STRPTR)device->parentName, parentUnit,
	               (struct IORequest *)&parent->openReq, 0))
	{
		FreeMem(parent, sizeof(struct VDriveParent));
		return NULL;
	}
	Forbid();
	AddTail((struct List *)&device->parents, (struct Node *)parent);
	Permit();
	return parent;
}

/**
 * vd_unit_new() - Allocate a virtual unit, offline.
 * @device: the device base (for SysBase)
 * @parent: the parent record it belongs to
 *
 * Change count 1, no held change interrupts, unit_MsgPort PA_IGNORE and unused.
 * A rescan brings it online. Task context.
 *
 * Return: the unit; NULL when AllocMem() fails
 */
struct VDUnit *vd_unit_new(struct VDrive *device, struct VDriveParent *parent)
{
	struct ExecBase *SysBase = device->SysBase;
	struct VDUnit *unit = AllocMem(sizeof(struct VDUnit), MEMF_PUBLIC | MEMF_CLEAR);
	if (unit)
	{
		unit->parent = parent;
		unit->changeCount = 1;
		new_list(&unit->changeInts);
		new_ignore_port(&unit->unit.unit_MsgPort);
	}
	return unit;
}

/**
 * vd_parents_free() - Close every parent unit held open and free the records.
 * @device: the device base, already off the device list, with no unit open and
 *          no wrapped request in flight
 *
 * Frees each record's units with it. Task context: CloseDevice() on the
 * parents.
 */
void vd_parents_free(struct VDrive *device)
{
	struct ExecBase *SysBase = device->SysBase;
	struct VDriveParent *parent;
	while ((parent = (struct VDriveParent *)RemHead((struct List *)&device->parents)))
	{
		CloseDevice((struct IORequest *)&parent->openReq);
		for (ULONG slot = 0; slot < VDRIVE_SLOTS; slot++)
			if (parent->units[slot])
				FreeMem(parent->units[slot], sizeof(struct VDUnit));
		FreeMem(parent, sizeof(struct VDriveParent));
	}
}

/**
 * vd_unit_view() - Take one consistent view of a unit.
 * @device: the device base
 * @unit:   the unit
 * @view:   written in full, under Forbid()
 *
 * Task context: a rescan changes these under Forbid() too, so the copy is of one
 * moment.
 */
void vd_unit_view(struct VDrive *device, const struct VDUnit *unit, struct VDView *view)
{
	struct ExecBase *SysBase = device->SysBase;
	Forbid();
	view->online = unit->online;
	view->start = unit->start;
	view->length = unit->length;
	view->changeCount = unit->changeCount;
	view->blockShift = unit->parent->blockShift;
	view->removable = unit->parent->removable;
	Permit();
}

/* The geometry brcm-emmc.device gives a virtual drive. */
#define GEOMETRY_HEADS          128
#define GEOMETRY_TRACK_SECTORS  64
#define GEOMETRY_CYLINDER       (GEOMETRY_HEADS * GEOMETRY_TRACK_SECTORS)

/**
 * vd_unit_geometry() - Build the geometry a virtual unit reports.
 * @view:     the unit's view (vd_unit_view())
 * @geometry: written in full
 *
 * As brcm-emmc does: GEOMETRY_HEADS heads of GEOMETRY_TRACK_SECTORS sectors, or
 * a flat one block a cylinder for drives too small to fill one such cylinder.
 * dg_Cylinders leaves out the blocks past the last whole cylinder;
 * dg_TotalSectors counts them. MEMF_PUBLIC buffers, DG_DIRECT_ACCESS,
 * DGF_REMOVABLE when the parent is removable.
 */
void vd_unit_geometry(const struct VDView *view, struct DriveGeometry *geometry)
{
	memset(geometry, 0, sizeof(*geometry));
	geometry->dg_SectorSize = 1UL << view->blockShift;
	geometry->dg_TotalSectors = view->length;
	if (view->length >= GEOMETRY_CYLINDER)
	{
		geometry->dg_Heads = GEOMETRY_HEADS;
		geometry->dg_TrackSectors = GEOMETRY_TRACK_SECTORS;
		geometry->dg_CylSectors = GEOMETRY_CYLINDER;
		geometry->dg_Cylinders = view->length / GEOMETRY_CYLINDER;
	}
	else
	{
		geometry->dg_Heads = 1;
		geometry->dg_TrackSectors = 1;
		geometry->dg_CylSectors = 1;
		geometry->dg_Cylinders = view->length;
	}
	geometry->dg_BufMemType = MEMF_PUBLIC;
	geometry->dg_DeviceType = DG_DIRECT_ACCESS;
	geometry->dg_Flags = view->removable ? DGF_REMOVABLE : 0;
}
