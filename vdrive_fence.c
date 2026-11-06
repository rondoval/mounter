/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * vdrive_fence.c — the write fence on a parent unit: there, the blocks of every
 * virtual drive that has a unit are read-only to everyone but the virtual
 * device, since no two paths may write the same blocks. The host asks on its
 * write paths, from a task, with the unit's handle (struct VDriveParent). No
 * I/O and no allocation here, only Forbid() around the fence read, the lock of
 * all the device's state (vdrive_internal.h).
 *
 * Part of the generic autoboot/automount mounter; see LICENSE for the
 * copyright and licence covering this file.
 */
#include "vdrive_internal.h"

/**
 * fence_applies() - Tell whether a request on a parent unit is to be checked.
 * @parent: the unit's handle; NULL accepted (no virtual drives)
 * @io:     the request; NULL accepted, never taken for one of the device's own
 *
 * No lock: one BOOL read, the fast path for a unit with nothing fenced. A
 * rescan fencing the unit meanwhile is seen by the next request.
 *
 * Return: TRUE when the unit has a fenced virtual drive and @io is not one of the
 * device's own forwarded requests (its mn_ReplyPort is not the forward port);
 * FALSE otherwise, @parent NULL included
 */
static BOOL fence_applies(const struct VDriveParent *parent, const struct IORequest *io)
{
	return parent && parent->fenced &&
	       !(io && io->io_Message.mn_ReplyPort == &parent->device->forwardPort);
}

/**
 * fence_clear() - Check a byte range against a parent's fence.
 * @parent: the parent record. Caller holds Forbid()
 * @start:  the range's first byte on the parent
 * @end:    one past its last byte
 *
 * The fence is the parent's fenced units, each the blocks of its virtual drive.
 *
 * Return: TRUE when [@start, @end) overlaps no fenced unit's blocks; FALSE when
 * it overlaps one
 */
static BOOL fence_clear(const struct VDriveParent *parent, uint64_t start, uint64_t end)
{
	const UBYTE shift = parent->blockShift;
	for (ULONG slot = 0; slot < VDRIVE_SLOTS; slot++)
	{
		const struct VDUnit *unit = parent->units[slot];
		if (!unit || !unit->fenced)
			continue;
		uint64_t first = unit->start << shift;
		uint64_t last = (unit->start + unit->length) << shift;
		if (start < last && end > first)
			return FALSE;
	}
	return TRUE;
}

/**
 * VDriveWriteAllowed() - Check a write on a parent unit against the fence.
 * @parent: the unit's handle (VDriveRefresh()); NULL accepted
 * @io:     the request being checked; NULL accepted (then never taken for one
 *          of the device's own)
 * @offset: its first byte on the unit
 * @length: its length in bytes; a range past 2^64 is cut there
 *
 * The host asks on every write, format, write-zeroes or trim, from a task, and
 * completes a refused one with TDERR_WriteProt.
 *
 * Return: TRUE if the request is one of the virtual device's own, @length is 0,
 * the unit has no fenced virtual drive (@parent NULL included), or the range
 * touches none; FALSE if it touches one
 */
BOOL VDriveWriteAllowed(struct VDriveParent *parent, const struct IORequest *io,
                        uint64_t offset, uint64_t length)
{
	if (!fence_applies(parent, io) || !length)
		return TRUE;
	struct ExecBase *SysBase = parent->device->SysBase;
	uint64_t end = offset + length;
	if (end < offset)
		end = ~(uint64_t)0;
	Forbid();
	BOOL allowed = fence_clear(parent, offset, end);
	Permit();
	return allowed;
}

/**
 * scsi_read_only() - Tell whether a SCSI opcode leaves the medium alone.
 * @opcode: the CDB's operation code
 *
 * The opcodes VDriveScsiAllowed() passes untouched on a fenced parent: the READ
 * and VERIFY commands; inquiries and the capacity, mode, log and status
 * queries; READ BUFFER, READ DEFECT DATA and RECEIVE DIAGNOSTIC RESULTS; cache
 * and medium control (SYNCHRONIZE CACHE, PRE-FETCH, START STOP UNIT, PREVENT
 * ALLOW MEDIUM REMOVAL); the MMC read-side queries. SEND DIAGNOSTIC is not one:
 * it carries a parameter list out.
 *
 * Return: TRUE for one of those; FALSE for anything else
 */
static BOOL scsi_read_only(UBYTE opcode)
{
	switch (opcode)
	{
	case SCSI_TEST_UNIT_READY:
	case SCSI_REQUEST_SENSE:
	case SCSI_READ_6:
	case SCSI_INQUIRY:
	case SCSI_MODE_SENSE_6:
	case SCSI_START_STOP_UNIT:
	case SCSI_RECEIVE_DIAGNOSTIC:
	case SCSI_PREVENT_ALLOW_REMOVAL:
	case SCSI_READ_FORMAT_CAPACITIES:
	case SCSI_READ_CAPACITY_10:
	case SCSI_READ_10:
	case SCSI_VERIFY_10:
	case SCSI_PREFETCH_10:
	case SCSI_SYNCHRONIZE_CACHE_10:
	case SCSI_READ_DEFECT_DATA_10:
	case SCSI_READ_BUFFER:
	case SCSI_READ_TOC:
	case SCSI_GET_CONFIGURATION:
	case SCSI_GET_EVENT_STATUS:
	case SCSI_LOG_SENSE:
	case SCSI_READ_DISC_INFORMATION:
	case SCSI_MODE_SENSE_10:
	case SCSI_READ_16:
	case SCSI_VERIFY_16:
	case SCSI_SYNCHRONIZE_CACHE_16:
	case SCSI_SERVICE_ACTION_IN_16:                 /* READ CAPACITY(16) and other queries */
	case SCSI_REPORT_LUNS:
	case SCSI_MAINTENANCE_IN:
	case SCSI_READ_12:
	case SCSI_VERIFY_12:
	case SCSI_READ_DEFECT_DATA_12:
	case SCSI_MECHANISM_STATUS:
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * scsi_write_clear() - Check a non-read-only SCSI command against a fence.
 * @parent: the parent record, with a unit fenced. Caller holds Forbid()
 * @scsi:   the command, with a CDB
 *
 * WRITE, WRITE AND VERIFY and WRITE SAME are range-checked in the parent's
 * blocks, a count of 0 reaching to the end of the medium; UNMAP is checked per
 * descriptor of its parameter list; ATA PASS-THROUGH (12) and (16) passes only
 * for data-in: PIO Data-In, or DMA or UDMA Data-In with T_DIR set (SAT).
 *
 * Return: TRUE for one of those that touches no virtual drive, or the data-in
 * ATA PASS-THROUGH; FALSE for the rest, and for anything else
 */
static BOOL scsi_write_clear(const struct VDriveParent *parent, const struct SCSICmd *scsi)
{
	const UBYTE *cdb = scsi->scsi_Command;
	const UWORD cdbLength = scsi->scsi_CmdLength;
	const UBYTE shift = parent->blockShift;
	struct VDCdbRange range;

	BOOL decoded = vd_cdb_range(cdb, cdbLength, &range);
	if (decoded && (range.kind == VD_CDB_WRITE || range.kind == VD_CDB_WRITE_SAME))
	{
		uint64_t end = range.blocks ? (range.lba + range.blocks) << shift : ~(uint64_t)0;
		return fence_clear(parent, range.lba << shift, end);
	}
	if (cdb[0] == SCSI_UNMAP && scsi->scsi_Data)    /* every descriptor */
	{
		const struct SCSIUnmapHeader *header = (const struct SCSIUnmapHeader *)scsi->scsi_Data;
		const struct SCSIUnmapDescriptor *descriptors = (const struct SCSIUnmapDescriptor *)(header + 1);
		ULONG room = scsi->scsi_Length >= sizeof(*header) ? scsi->scsi_Length - sizeof(*header) : 0;
		ULONG bytes = room ? header->descriptorsLength : 0;
		if (bytes > room)
			bytes = room;
		for (ULONG i = 0; i < bytes / sizeof(*descriptors); i++)
		{
			uint64_t lba = descriptors[i].lba;
			if (!fence_clear(parent, lba << shift, (lba + descriptors[i].blocks) << shift))
				return FALSE;
		}
		return TRUE;
	}
	if ((cdb[0] == SCSI_ATA_PASS_THROUGH_16 || cdb[0] == SCSI_ATA_PASS_THROUGH_12) &&
	    cdbLength >= sizeof(struct SCSICdbAtaPassThrough))
	{
		const struct SCSICdbAtaPassThrough *ata = (const struct SCSICdbAtaPassThrough *)cdb;
		UBYTE protocol = SCSI_ATA_PROTOCOL(ata->protocol);
		BOOL dataIn = (ata->flags & SCSI_ATA_T_DIR) != 0;
		return protocol == SCSI_ATA_PIO_IN ||
		       ((protocol == SCSI_ATA_DMA || protocol == SCSI_ATA_UDMA_IN) && dataIn);
	}
	return FALSE;
}

/**
 * VDriveScsiAllowed() - Check an HD_SCSICMD on a parent unit against the fence.
 * @parent: the unit's handle (VDriveRefresh()); NULL accepted
 * @io:     the request being checked; NULL accepted (then never taken for one
 *          of the device's own)
 * @scsi:   the command (io_Data); NULL, or one without a CDB, passes. On FALSE
 *          scsi_Status and the autosense data are written
 *
 * The same as VDriveWriteAllowed() for an HD_SCSICMD on the parent: reads and
 * queries (scsi_read_only()) pass, writes pass when they touch no fenced
 * virtual drive (scsi_write_clear()), and anything else is refused while the
 * unit has one. The host asks from a task, and completes a refused command
 * with HFERR_BadStatus.
 *
 * Return: TRUE if the command passes, or is one of the virtual device's own;
 * FALSE if refused, with the CHECK CONDITION status and DATA PROTECT, write
 * protected (07/27h/00) sense already in @scsi (vd_scsi_check())
 */
BOOL VDriveScsiAllowed(struct VDriveParent *parent, const struct IORequest *io,
                       struct SCSICmd *scsi)
{
	if (!fence_applies(parent, io) || !scsi || !scsi->scsi_Command || !scsi->scsi_CmdLength)
		return TRUE;
	struct ExecBase *SysBase = parent->device->SysBase;

	Forbid();
	BOOL allowed = !parent->fenced || scsi_read_only(scsi->scsi_Command[0]) ||
	               scsi_write_clear(parent, scsi);
	Permit();

	if (!allowed)
		vd_scsi_check(scsi, VD_SENSE_WRITE_PROTECTED);
	return allowed;
}

/**
 * VDriveAnyFenced() - Tell whether any parent unit has a fenced virtual drive.
 * @device: the device; NULL accepted
 *
 * For whole-device commands, which no per-unit fence can check. No lock: one
 * ULONG read.
 *
 * Return: TRUE if any parent unit has a fenced virtual drive; FALSE if none
 * has, or @device is NULL
 */
BOOL VDriveAnyFenced(struct VDrive *device)
{
	return device && device->fencedParents != 0;
}
