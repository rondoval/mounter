
// Generic autoboot/automount RDB parser and mounter.
// - AmigaOS 3.1 (V40) and up.
// - 68000 compatible.
// - Mounts both pre-DOS (from a ROM-resident driver, where the nodes it adds
//   are what strap boots from) and post-DOS (hotplug).
// - Full automount support
// - Full RDB filesystem support.
//
// This unit holds the two ways in: MountDrive() sets up the call and probes each
// unit, handing the medium to the scanner for its kind, and
// mnt_find_virtual_drives() lists a unit's virtual drives for the virtual-drive
// device. ARCHITECTURE.md maps the rest.
//
// Copyright 2021-2022 Toni Wilen
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
#include "mounter_internal.h"

#define DOSTYPE_FAT95 0x46415401        /* FAT\1 */
#define DOSTYPE_CD01  0x43443031        /* CD01 */
#define DOSTYPE_CDVD  0x43445644        /* CDVD */

// The classic recipes, for a caller that gives none: FAT95, and whichever CD
// filesystem FileSystem.resource carries, from the resource alone.
static const struct MountFS classicFAT  = { .dosType = DOSTYPE_FAT95 };
static const struct MountFS classicCD01 = { .dosType = DOSTYPE_CD01 };
static const struct MountFS classicCDVD = { .dosType = DOSTYPE_CDVD };

/**
 * set_recipes() - Take the caller's recipes, filling in the classic ones.
 * @md: mount state; md->fs[] receives the recipes
 * @ms: the caller's request; ms->fs[] may hold NULLs
 *
 * A NULL entry means that family is not mounted, except FAT, which falls back to
 * the FAT95 dostype, and CD, which falls back to CD01 or CDVD, whichever
 * FileSystem.resource carries first (CD01 when neither). Both from the resource
 * only, with no flags: an ISO9660-only filesystem that cannot present audio
 * discs. The scanners never see a NULL FAT or CD recipe.
 */
static void set_recipes(struct MountData *md, const struct MountStruct *ms)
{
	for (int i = 0; i < MOUNTFS_KINDS; i++)
		md->fs[i] = ms->fs[i];
	if (!md->fs[MOUNTFS_FAT])
		md->fs[MOUNTFS_FAT] = &classicFAT;
	if (!md->fs[MOUNTFS_CD]) {
		struct FileSysEntry *fse = mnt_find_filesystem(DOSTYPE_CD01, DOSTYPE_CDVD, md->SysBase);
		md->fs[MOUNTFS_CD] = (fse && fse->fse_DosType == DOSTYPE_CDVD) ? &classicCDVD : &classicCD01;
	}
}

/**
 * md_new() - Allocate the mount state for one device, with its I/O request.
 * @SysBase:    exec
 * @deviceName: the device the request is for
 *
 * The rest of the state is zeroed. Task context: the request's reply port is the
 * calling task's.
 *
 * Return: the state, md->request not open yet; NULL when memory, the message
 * port or the I/O request cannot be had
 */
static struct MountData *md_new(struct ExecBase *SysBase, const UBYTE *deviceName)
{
	struct MountData *md = AllocMem(sizeof(struct MountData), MEMF_CLEAR | MEMF_PUBLIC);
	if (!md)
		return NULL;
	md->SysBase = SysBase;
	md->devicename = deviceName;
	struct MsgPort *port = CreateMsgPort();
	md->request = port ? (struct IOExtTD *)CreateIORequest(port, sizeof(struct IOExtTD)) : NULL;
	if (!md->request) {
		if (port)
			DeleteMsgPort(port);
		FreeMem(md, sizeof(struct MountData));
		return NULL;
	}
	return md;
}

/**
 * md_free() - Free what md_new() made, and the scratch sectors.
 * @md: the state; md->request not open
 */
static void md_free(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct MsgPort *port = md->request->iotd_Req.io_Message.mn_ReplyPort;
	DeleteIORequest(md->request);
	DeleteMsgPort(port);
	mnt_sector_pool_free(md);
	FreeMem(md, sizeof(struct MountData));
}

/**
 * read_geometry() - Ask the unit for its geometry with TD_GETGEOMETRY.
 * @md:       mount state; md->request is open on the unit
 * @geometry: receives the unit's DriveGeometry
 *
 * Return: 0 on success; else the io_Error DoIO() returns
 */
static BYTE read_geometry(struct MountData *md, struct DriveGeometry *geometry)
{
	struct ExecBase *SysBase = md->SysBase;
	struct IOExtTD *req = md->request;

	req->iotd_Req.io_Command = TD_GETGEOMETRY;
	req->iotd_Req.io_Data    = geometry;
	req->iotd_Req.io_Length  = sizeof(struct DriveGeometry);

	return DoIO((struct IORequest *)req);
}

/**
 * close_unit() - Turn the unit's motor off and close md->request.
 * @md: mount state; md->request is open on the unit
 */
static void close_unit(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct IOExtTD *request = md->request;

	request->iotd_Req.io_Command = TD_MOTOR;
	request->iotd_Req.io_Length  = 0;
	DoIO((struct IORequest*)request);
	CloseDevice((struct IORequest*)request);
}

/**
 * open_unit() - Open md->request on a unit and read its geometry.
 * @md:      mount state; md->request is not open
 * @unitNum: the unit
 * @geom:    receives the unit's DriveGeometry
 *
 * md->blocksize and md->totalsectors are taken from @geom; whether the mounter
 * reads blocks of that size (block_size_ok()) is the caller's to check.
 *
 * Return: TRUE with the unit open; FALSE when OpenDevice() fails, or
 * TD_GETGEOMETRY does and the unit is closed again (close_unit())
 */
static BOOL open_unit(struct MountData *md, ULONG unitNum, struct DriveGeometry *geom)
{
	struct ExecBase *SysBase = md->SysBase;

	dbg("OpenDevice('%s', %ld, 0x%08lx, 0)\n", md->devicename, unitNum, (ULONG)md->request);
	BYTE err = OpenDevice(md->devicename, unitNum, (struct IORequest*)md->request, 0);
	if (err != 0) {
		dbg("OpenDevice(%s,%ld) failed: %ld\n", md->devicename, unitNum, (LONG)err);
		return FALSE;
	}
	if (read_geometry(md, geom) != 0) {
		close_unit(md);
		return FALSE;
	}
	md->blocksize    = geom->dg_SectorSize;
	md->totalsectors = geom->dg_TotalSectors;
	return TRUE;
}

/**
 * block_size_ok() - Tell whether the mounter reads blocks of this size.
 * @size: dg_SectorSize
 *
 * Return: TRUE for 256..MAX_BLOCKSIZE; FALSE otherwise
 */
static BOOL block_size_ok(ULONG size)
{
	return size >= 256 && size <= MAX_BLOCKSIZE;
}

/**
 * probe_unit() - Open one unit, scan its medium and close it again.
 * @md:      mount state; md->request is the I/O request, not open, and the
 *           per-unit fields (unitnum, blocksize, totalsectors, mounted,
 *           wasLastDev, table) are set here
 * @unitNum: unit to open
 *
 * The scanners answer one question each — did you recognize this medium? — and
 * leave the count in md->mounted, so the -1/0/count encoding is applied here,
 * once. A direct-access unit has its partition table read first, which the RDB
 * scan needs as well as the legacy one; then mnt_scan_rdb() runs, and
 * mnt_scan_legacy() if that recognized nothing. A CD-ROM, WORM or optical unit
 * goes to mnt_scan_cd(). MSF_NO_RDB, MSF_NO_LEGACY and MSF_NO_CD turn the
 * scanners off.
 *
 * Return: -1 if nothing recognized the medium, including when OpenDevice() or
 * TD_GETGEOMETRY fails, dg_SectorSize is outside 256..MAX_BLOCKSIZE or no
 * scanner takes the device type; else the number mounted (md->mounted, extents
 * found already mounted included; 0 = recognized but nothing mounted)
 */
static LONG probe_unit(struct MountData *md, ULONG unitNum)
{
	struct DriveGeometry geom;
	BOOL recognized = FALSE;

	md->unitnum      = unitNum;
	md->mounted      = 0;
	md->wasLastDev   = FALSE;
	md->table.scheme = TABLE_NONE;
	md->table.count  = 0;

	if (!open_unit(md, unitNum, &geom))
		return -1;
	if (!block_size_ok(geom.dg_SectorSize)) {
		mnt_log("Unsupported sector size %lu.\n", geom.dg_SectorSize);
	} else {
		switch (geom.dg_DeviceType & SID_TYPE) {
		case DG_CDROM:
		case DG_WORM:
		case DG_OPTICAL_DISK:
			if (!(md->flags & MSF_NO_CD))
				recognized = mnt_scan_cd(md);
			else {
				mnt_log("CDROM mounting disabled.\n");
			}
			break;
		case DG_DIRECT_ACCESS:
			mnt_read_table(md);
			if (!(md->flags & MSF_NO_RDB))
				recognized = mnt_scan_rdb(md);
			if (!recognized && !(md->flags & MSF_NO_LEGACY))
				recognized = mnt_scan_legacy(md);
			break;
		default:
			mnt_log("Don't know how to boot from device type %ld.\n", (LONG)(geom.dg_DeviceType & SID_TYPE));
			break;
		}
	}
	// Every scanner releases what it takes; a leak would shrink the pool for the
	// units still to come.
	if (md->sectorsUsed) {
		mnt_log("Sector pool leak: %ld slot(s)\n", (LONG)md->sectorsUsed);
		md->sectorsUsed = 0;
	}
	close_unit(md);
	return recognized ? md->mounted : -1;
}

/**
 * scan_units() - Probe each unit the caller listed, in order.
 * @md: mount state, shared by all the units; md->total and md->recognized add up
 *      the units' results
 * @ms: the caller's request: ms->units[], ms->unitCount, and ms->unitResults
 *      (NULL if not wanted)
 *
 * Explicit units let a caller mount known units (e.g. a hotplug driver) instead
 * of scanning. Each ms->unitResults[i] gets that unit's probe_unit() result, or
 * -2 when the unit was skipped after a prior RDBFF_LAST (ignored with
 * MSF_IGNORE_LAST).
 */
static void scan_units(struct MountData *md, const struct MountStruct *ms)
{
	BOOL skipRest = FALSE;

	for (ULONG i = 0; i < ms->unitCount; i++) {
		LONG r = -2;                    /* skipped after an earlier RDBFF_LAST */
		if (!skipRest) {
			r = probe_unit(md, ms->units[i]);
			if (r >= 0) {
				md->recognized = TRUE;
				md->total += r;
			}
			if (md->wasLastDev && !(md->flags & MSF_IGNORE_LAST)) {
				dbg("RDBFF_LAST exit\n");
				skipRest = TRUE;
			}
		}
		if (ms->unitResults)
			ms->unitResults[i] = r;
	}
}

/**
 * MountDrive() - Mount the volumes on the given units of one device.
 * @ms:  what to mount: device, units, flags, recipes; never written, except
 *       through ms->unitResults (-1/0/count per unit, -2 = skipped because a
 *       previous unit had RDBFF_LAST set)
 * @res: zeroed first, then filled with the per-call breakdown; NULL if not wanted
 *
 * The one way in; struct MountStruct and struct MountResult in mounter.h describe
 * the fields. md->DOSBase stays NULL while dos.library (V40) cannot be opened,
 * and that is what selects the pre-DOS paths throughout. Before DOS exists a
 * bootable node needs a ConfigDev to be an NT_BOOTNODE at all; the mount makes a
 * fake one on the first such node unless the caller has a real board
 * (ms->configDev).
 *
 * Return: >0, the volumes mounted across all units (extents found already
 * mounted included); 0 if a unit's medium was recognized but nothing mounted;
 * -1 if nothing was recognized on any unit, no units were given, or setup failed
 * (expansion.library V40, memory, message port or I/O request)
 */
LONG MountDrive(const struct MountStruct *ms, struct MountResult *res)
{
	struct ExecBase *SysBase = ms->SysBase;
	struct ExpansionBase *ExpansionBase = NULL;
	struct MountData *md = NULL;
	LONG ret = -1;

	dbg("Starting..\n");

	if (res)
		memset(res, 0, sizeof(*res));

	if (!ms->units || !ms->unitCount) {
		mnt_log("MountDrive: no units given\n");
		return -1;
	}

	ExpansionBase = (struct ExpansionBase*)OpenLibrary((CONST_STRPTR)"expansion.library", 40);
	if (!ExpansionBase)
		goto cleanup;

	md = md_new(SysBase, ms->deviceName);
	if (!md)
		goto cleanup;

	md->ExpansionBase = ExpansionBase;
	md->DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 40);
	dbg("SysBase=0x%08lx ExpansionBase=0x%08lx DosBase=0x%08lx\n", (ULONG)md->SysBase, (ULONG)md->ExpansionBase, (ULONG)md->DOSBase);
	md->creator = ms->creatorName ? ms->creatorName : ms->deviceName;
	md->flags = ms->flags;
	md->dmaAlign = ms->dmaAlign;
	md->configDev = ms->configDev;
	set_recipes(md, ms);

	scan_units(md, ms);
	ret = md->total > 0 ? md->total : (md->recognized ? 0 : -1);

cleanup:
	if (md) {
		if (res) {
			res->mounted        = md->total;
			res->deferred       = md->deferred;
			res->alreadyMounted = md->alreadyMounted;
			res->renamed        = md->renamed;
			res->recognized     = md->recognized;
		}
		if (md->DOSBase)
			CloseLibrary(&md->DOSBase->dl_lib);
		md_free(md);
	}
	if (ExpansionBase)
		CloseLibrary(&ExpansionBase->LibNode);

	dbg("Exit code %ld\n", ret);
	return ret;
}

/**
 * mnt_find_virtual_drives() - List the virtual drives on one unit.
 * @SysBase:    exec
 * @deviceName: the device
 * @unitNum:    the unit; opened and closed again here, from the calling task
 * @geom:       receives the unit's DriveGeometry, when the unit opens and
 *              answers TD_GETGEOMETRY
 * @out:        room for @max entries: the partition table's own entries of
 *              kind PART_VIRTUAL, in table order
 * @max:        the entries @out holds
 *
 * The same open, geometry and partition table as the mount pass, on a state of
 * its own: no MountDrive() call is needed. Task context.
 *
 * Return: the number of virtual drives in the table, which may be more than @max
 * (only the first @max are written); 0 if there is none, or the medium cannot
 * hold one: not direct access, or a block size outside 256..MAX_BLOCKSIZE; -1
 * if memory runs out, the unit cannot be opened, or TD_GETGEOMETRY or block 0
 * fails
 */
LONG mnt_find_virtual_drives(struct ExecBase *SysBase, const UBYTE *deviceName, ULONG unitNum,
                             struct DriveGeometry *geom, struct Partition *out, ULONG max)
{
	struct MountData *md = md_new(SysBase, deviceName);
	if (!md)
		return -1;

	LONG found = -1;
	if (open_unit(md, unitNum, geom)) {
		BOOL disk = (geom->dg_DeviceType & SID_TYPE) == DG_DIRECT_ACCESS &&
		            block_size_ok(geom->dg_SectorSize);
		if (!disk) {
			found = 0;
		} else if (mnt_read_table(md)) {
			found = 0;
			for (UWORD i = 0; i < md->table.count; i++) {
				const struct Partition *p = &md->table.part[i];
				if (p->kind != PART_VIRTUAL)
					continue;
				if ((ULONG)found < max)
					out[found] = *p;
				found++;
			}
		}
		close_unit(md);
	}
	md_free(md);
	return found;
}
