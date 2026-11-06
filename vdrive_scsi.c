/*
 * vdrive_scsi.c — the SCSI target a virtual unit emulates behind HD_SCSICMD, for
 * HDToolBox and other partitioning tools, and the CDB decoder the parent fence
 * shares. No I/O here: a command that needs the parent comes back as a drafted
 * wrapped request for the caller to forward. The wire formats are
 * vdrive_scsi.h's.
 *
 * Part of the generic autoboot/automount mounter; see mounter.c for the
 * copyright and licence covering this file.
 */
#include "vdrive_internal.h"

#include <stddef.h>

/**
 * sense_build() - Build fixed-format sense data.
 * @data:  written in full
 * @sense: the sense key, ASC and ASCQ (VD_SENSE())
 *
 * A current error (SCSI_SENSE_CURRENT); every field the sense does not name is
 * zero.
 */
static void sense_build(struct SCSISense *data, ULONG sense)
{
	*data = (struct SCSISense){
		.responseCode = SCSI_SENSE_CURRENT,
		.key = VD_SENSE_KEY(sense),
		.additionalLength = sizeof(struct SCSISense) - offsetof(struct SCSISense, commandInformation),
		.asc = VD_SENSE_ASC(sense),
		.ascq = VD_SENSE_ASCQ(sense),
	};
}

/**
 * vd_scsi_check() - Set CHECK CONDITION and the autosense data in an HD_SCSICMD.
 * @scsi:  the command: scsi_Status becomes SCSI_STATUS_CHECK_CONDITION; with
 *         SCSIF_AUTOSENSE, a scsi_SenseData buffer and a nonzero
 *         scsi_SenseLength the sense goes there, as much as fits, and
 *         scsi_SenseActual counts it, else scsi_SenseActual is 0
 * @sense: the sense key, ASC and ASCQ (VD_SENSE())
 *
 * Neither io_Error nor a unit's REQUEST SENSE data is touched: the fence uses
 * it as it is, the emulation through vd_scsi_fail().
 */
void vd_scsi_check(struct SCSICmd *scsi, ULONG sense)
{
	scsi->scsi_Status = SCSI_STATUS_CHECK_CONDITION;
	scsi->scsi_SenseActual = 0;
	if ((scsi->scsi_Flags & SCSIF_AUTOSENSE) && scsi->scsi_SenseData && scsi->scsi_SenseLength)
	{
		struct SCSISense data;
		sense_build(&data, sense);
		UWORD count = scsi->scsi_SenseLength < sizeof(data) ? scsi->scsi_SenseLength
		                                                   : (UWORD)sizeof(data);
		memcpy(scsi->scsi_SenseData, &data, count);
		scsi->scsi_SenseActual = count;
	}
}

/**
 * vd_scsi_fail() - Fail an emulated HD_SCSICMD with CHECK CONDITION.
 * @unit:  the unit; its REQUEST SENSE data becomes @sense
 * @io:    the request; io_Error becomes HFERR_BadStatus
 * @scsi:  the command (io_Data); vd_scsi_check() sets its status and autosense
 * @sense: the sense key, ASC and ASCQ (VD_SENSE())
 *
 * Return: TRUE, for vd_scsi_command() to return: the command finished here
 */
BOOL vd_scsi_fail(struct VDUnit *unit, struct IOStdReq *io, struct SCSICmd *scsi, ULONG sense)
{
	unit->sense = sense;
	vd_scsi_check(scsi, sense);
	io->io_Error = HFERR_BadStatus;
	return TRUE;
}

/* A CDB's length by its group code (SCSI_GROUP()); groups 3, 6 and 7 are
 * reserved or vendor-specific, and none of those is emulated. */
static const UBYTE cdb_size[SCSI_GROUPS] = {
	sizeof(struct SCSICdb6),            /* group 0: 00h-1Fh */
	sizeof(struct SCSICdb10),           /* group 1: 20h-3Fh */
	sizeof(struct SCSICdb10),           /* group 2: 40h-5Fh */
	sizeof(struct SCSICdb6),            /* group 3: 60h-7Fh, reserved */
	sizeof(struct SCSICdb16),           /* group 4: 80h-9Fh */
	sizeof(struct SCSICdb12),           /* group 5: A0h-BFh */
	sizeof(struct SCSICdb6),            /* group 6: C0h-DFh, vendor-specific */
	sizeof(struct SCSICdb6),            /* group 7: E0h-FFh, vendor-specific */
};

/**
 * cdb_kind() - Tell what a CDB does with the block range it names.
 * @opcode: the CDB's operation code
 *
 * Where the range sits follows from the CDB's length (cdb_size[]).
 *
 * Return: VD_CDB_READ, VD_CDB_WRITE (WRITE AND VERIFY included), VD_CDB_VERIFY
 * or VD_CDB_WRITE_SAME; VD_CDB_NONE when it names no block range
 */
static UBYTE cdb_kind(UBYTE opcode)
{
	switch (opcode)
	{
	case SCSI_READ_6:
	case SCSI_READ_10:
	case SCSI_READ_12:
	case SCSI_READ_16:
		return VD_CDB_READ;
	case SCSI_WRITE_6:
	case SCSI_WRITE_10:
	case SCSI_WRITE_12:
	case SCSI_WRITE_16:
	case SCSI_WRITE_VERIFY_10:
	case SCSI_WRITE_VERIFY_12:
	case SCSI_WRITE_VERIFY_16:
		return VD_CDB_WRITE;
	case SCSI_VERIFY_10:
	case SCSI_VERIFY_12:
	case SCSI_VERIFY_16:
		return VD_CDB_VERIFY;
	case SCSI_WRITE_SAME_10:
	case SCSI_WRITE_SAME_16:
		return VD_CDB_WRITE_SAME;
	default:
		return VD_CDB_NONE;
	}
}

/**
 * vd_cdb_range() - Decode the block range of a READ, WRITE, VERIFY or WRITE SAME.
 * @cdb:    the CDB, @length bytes
 * @length: its length (scsi_CmdLength); 0 accepted
 * @range:  the result, written only on TRUE
 *
 * The CDB decoder shared by the emulation (vd_scsi_command()) and the parent
 * fence (VDriveScsiAllowed()). Known: READ and WRITE (6), (10), (12), (16);
 * WRITE AND VERIFY and VERIFY (10), (12), (16); WRITE SAME (10), (16). A 6-byte
 * CDB's transfer length 0 is SCSI_CDB6_BLOCKS_0 blocks; every other count is
 * taken as given. Every VERIFY counts as moving no blocks, whatever its BYTCHK.
 *
 * Return: TRUE when @cdb is one of those and @length holds all of it; FALSE
 * otherwise, @length 0 included
 */
BOOL vd_cdb_range(const UBYTE *cdb, UWORD length, struct VDCdbRange *range)
{
	if (!length)
		return FALSE;
	UBYTE kind = cdb_kind(cdb[0]);
	UBYTE size = cdb_size[SCSI_GROUP(cdb[0])];
	if (kind == VD_CDB_NONE || length < size)
		return FALSE;
	range->kind = kind;
	switch (size)
	{
	case sizeof(struct SCSICdb6):
	{
		const struct SCSICdb6 *cdb6 = (const struct SCSICdb6 *)cdb;
		range->lba = ((ULONG)(cdb6->lbaHigh & SCSI_CDB6_LBA_HIGH) << 16) | cdb6->lbaLow;
		range->blocks = cdb6->length ? cdb6->length : SCSI_CDB6_BLOCKS_0;
		range->flags = 0;
		break;
	}
	case sizeof(struct SCSICdb10):
	{
		const struct SCSICdb10 *cdb10 = (const struct SCSICdb10 *)cdb;
		range->lba = cdb10->lba;
		range->blocks = cdb10->length;
		range->flags = cdb10->flags;
		break;
	}
	case sizeof(struct SCSICdb12):
	{
		const struct SCSICdb12 *cdb12 = (const struct SCSICdb12 *)cdb;
		range->lba = cdb12->lba;
		range->blocks = cdb12->length;
		range->flags = cdb12->flags;
		break;
	}
	default:
	{
		const struct SCSICdb16 *cdb16 = (const struct SCSICdb16 *)cdb;
		range->lba = cdb16->lba;
		range->blocks = cdb16->length;
		range->flags = cdb16->flags;
		break;
	}
	}
	return TRUE;
}

/**
 * mode_pages() - Build the MODE SENSE pages a unit answers with.
 * @view:  the unit's view, whose geometry (vd_unit_geometry()) the pages give
 * @page:  the CDB's page byte; its page code (SCSI_MODE_PAGE_CODE) is
 *         SCSI_PAGE_FORMAT, SCSI_PAGE_GEOMETRY or SCSI_PAGE_ALL (both)
 * @pages: where they go: room for a SCSIFormatPage and a SCSIGeometryPage
 *
 * The format device page carries sectors per track and bytes per sector, the
 * rigid disk geometry page the cylinders (low 24 bits) and heads; the rest is
 * zero.
 *
 * Return: the bytes built; 0 for any other page code
 */
static ULONG mode_pages(const struct VDView *view, UBYTE page, UBYTE *pages)
{
	page &= SCSI_MODE_PAGE_CODE;
	if (page != SCSI_PAGE_FORMAT && page != SCSI_PAGE_GEOMETRY && page != SCSI_PAGE_ALL)
		return 0;
	struct DriveGeometry geometry;
	vd_unit_geometry(view, &geometry);
	ULONG length = 0;
	if (page == SCSI_PAGE_FORMAT || page == SCSI_PAGE_ALL)
	{
		const struct SCSIFormatPage format = {
			.header = {
				.code = SCSI_PAGE_FORMAT,
				.length = sizeof(struct SCSIFormatPage) - sizeof(struct SCSIPageHeader),
			},
			.sectorsPerTrack = (UWORD)geometry.dg_TrackSectors,
			.bytesPerSector = (UWORD)geometry.dg_SectorSize,
		};
		memcpy(pages + length, &format, sizeof(format));
		length += sizeof(format);
	}
	if (page == SCSI_PAGE_GEOMETRY || page == SCSI_PAGE_ALL)
	{
		const struct SCSIGeometryPage rigid = {
			.header = {
				.code = SCSI_PAGE_GEOMETRY,
				.length = sizeof(struct SCSIGeometryPage) - sizeof(struct SCSIPageHeader),
			},
			.cylindersHigh = (UBYTE)(geometry.dg_Cylinders >> 16),
			.cylinders = (UWORD)geometry.dg_Cylinders,
			.heads = (UBYTE)geometry.dg_Heads,
		};
		memcpy(pages + length, &rigid, sizeof(rigid));
		length += sizeof(rigid);
	}
	return length;
}

/**
 * capacity_check() - Check a READ CAPACITY's LBA and find the block to return.
 * @view: the unit's view
 * @pmi:  the CDB's PMI bit (SCSI_READ_CAPACITY_PMI)
 * @lba:  the CDB's LBA
 * @last: written on success: the block READ CAPACITY returns
 *
 * Without PMI the LBA must be 0, and the answer is the unit's last block. With
 * PMI, SBC-2 asks for the last block after @lba before a substantial delay,
 * which HDToolBox reads as the last block of the track holding @lba (NDK
 * HDToolBox release notes): that is the answer, the track laid out as
 * vd_unit_geometry() does, cut at the unit's last block.
 *
 * Return: VD_SENSE_NONE on success; VD_SENSE_BAD_FIELD for an LBA without PMI,
 * VD_SENSE_BAD_LBA for one past the unit's last block
 */
static ULONG capacity_check(const struct VDView *view, BOOL pmi, uint64_t lba, ULONG *last)
{
	const ULONG unitLast = view->length - 1;
	if (!pmi)
	{
		*last = unitLast;
		return lba ? VD_SENSE_BAD_FIELD : VD_SENSE_NONE;
	}
	if (lba > unitLast)
		return VD_SENSE_BAD_LBA;
	struct DriveGeometry geometry;
	vd_unit_geometry(view, &geometry);
	ULONG trackEnd = ((ULONG)lba / geometry.dg_TrackSectors + 1) * geometry.dg_TrackSectors - 1;
	*last = trackEnd < unitLast ? trackEnd : unitLast;
	return VD_SENSE_NONE;
}

/* The MODE SENSE responses: the mode parameter header, then the pages. */
struct ModeSense6Data
{
	struct SCSIModeHeader6 header;
	UBYTE pages[sizeof(struct SCSIFormatPage) + sizeof(struct SCSIGeometryPage)];
} __attribute__((packed));

struct ModeSense10Data
{
	struct SCSIModeHeader10 header;
	UBYTE pages[sizeof(struct SCSIFormatPage) + sizeof(struct SCSIGeometryPage)];
} __attribute__((packed));

/**
 * response_copy() - Copy an emulated command's response to scsi_Data.
 * @scsi:       the command; scsi_Actual becomes the bytes copied
 * @data:       the response
 * @length:     its length in bytes
 * @allocation: the CDB's allocation length
 *
 * Copies the least of @length, @allocation and scsi_Length; with a NULL
 * scsi_Data, nothing.
 */
static void response_copy(struct SCSICmd *scsi, const void *data, ULONG length, ULONG allocation)
{
	ULONG count = length < allocation ? length : allocation;
	if (count > scsi->scsi_Length)
		count = scsi->scsi_Length;
	if (!scsi->scsi_Data)
		count = 0;
	if (count)
		memcpy(scsi->scsi_Data, data, count);
	scsi->scsi_Actual = count;
}

/* The standard INQUIRY data of every virtual unit. */
static const struct SCSIInquiry inquiry_data = {
	.deviceType = SCSI_TYPE_DIRECT_ACCESS,
	.removable = 0,
	.version = SCSI_VERSION_SPC3,
	.responseFormat = SCSI_INQUIRY_FORMAT,
	.additionalLength = sizeof(struct SCSIInquiry) - offsetof(struct SCSIInquiry, flags),
	.vendor = "VIRTUAL ",
	.product = "Virtual drive   ",
	.revision = "1.0 ",
};

/**
 * vd_scsi_command() - Emulate an HD_SCSICMD on a virtual unit.
 * @unit:  the unit
 * @io:    the request: io_Data is a struct SCSICmd, io_Length at least its
 *         size (the caller checks both); io_Error is written
 * @view:  the unit's view, taken by the caller
 * @draft: drafted in full when the command goes to the parent
 *
 * TEST UNIT READY, START STOP UNIT and PREVENT ALLOW MEDIUM REMOVAL succeed
 * doing nothing; REQUEST SENSE returns the unit's sense and clears it; INQUIRY
 * gives standard data only; READ CAPACITY (10) and (16) answer as
 * capacity_check() says; MODE SENSE (6) and (10) pages 3, 4 and 3Fh.
 * SYNCHRONIZE CACHE (10) and (16) go to the parent as CMD_UPDATE; READ, WRITE
 * and WRITE AND VERIFY (no verify) as NSCMD_TD_READ64 or NSCMD_TD_WRITE64 at
 * (start + LBA) << shift, once the range (LBA out of range, 05/21h/00) and the
 * buffer (invalid field in CDB, 05/24h/00) pass; VERIFY only checks the range,
 * and with BYTCHK set is refused (05/24h/00): no compare is done here. A CDB
 * shorter than its group code says, INQUIRY with EVPD and an unsupported mode
 * page are invalid field in CDB (05/24h/00); anything else, WRITE SAME
 * included, is ILLEGAL REQUEST (05/20h/00). While offline only INQUIRY and
 * REQUEST SENSE answer; the rest is NOT READY, medium not present (02/3Ah/00).
 *
 * scsi_CmdActual is set to the CDB length; scsi_Actual, scsi_SenseActual and
 * scsi_Status start at 0.
 *
 * Return: TRUE if the command finished here and the caller replies @io:
 * io_Error 0, or HFERR_BadStatus with CHECK CONDITION, the autosense data and
 * the unit's REQUEST SENSE data set (vd_scsi_fail()); FALSE if it goes to the
 * parent as @draft, and vd_scsi_done() completes it
 */
BOOL vd_scsi_command(struct VDUnit *unit, struct IOStdReq *io, const struct VDView *view,
                     struct VDWrappedIO *draft)
{
	struct SCSICmd *scsi = (struct SCSICmd *)io->io_Data;
	const UBYTE *cdb = scsi->scsi_Command;
	UWORD cdbLength = scsi->scsi_CmdLength;
	const UBYTE shift = view->blockShift;
	const ULONG capacity = view->length;
	struct VDCdbRange range;

	scsi->scsi_Actual = 0;
	scsi->scsi_SenseActual = 0;
	scsi->scsi_CmdActual = cdbLength;
	scsi->scsi_Status = SCSI_STATUS_GOOD;
	io->io_Error = 0;
	if (!cdb || !cdbLength)
		return vd_scsi_fail(unit, io, scsi, VD_SENSE_BAD_OPCODE);
	UBYTE opcode = cdb[0];
	if (cdbLength < cdb_size[SCSI_GROUP(opcode)])
		return vd_scsi_fail(unit, io, scsi, VD_SENSE_BAD_FIELD);  /* a cut-off CDB */
	if (!view->online && opcode != SCSI_INQUIRY && opcode != SCSI_REQUEST_SENSE)
		return vd_scsi_fail(unit, io, scsi, VD_SENSE_NOT_PRESENT);

	switch (opcode)
	{
	case SCSI_TEST_UNIT_READY:
	case SCSI_START_STOP_UNIT:
	case SCSI_PREVENT_ALLOW_REMOVAL:
		return TRUE;
	case SCSI_REQUEST_SENSE:
	{
		const struct SCSICdbRequestSense *request = (const struct SCSICdbRequestSense *)cdb;
		struct SCSISense data;
		sense_build(&data, unit->sense);
		response_copy(scsi, &data, sizeof(data), request->allocationLength);
		unit->sense = VD_SENSE_NONE;
		return TRUE;
	}
	case SCSI_INQUIRY:
	{
		const struct SCSICdbInquiry *request = (const struct SCSICdbInquiry *)cdb;
		if (request->flags & SCSI_INQUIRY_EVPD)
			return vd_scsi_fail(unit, io, scsi, VD_SENSE_BAD_FIELD);  /* standard data only */
		response_copy(scsi, &inquiry_data, sizeof(inquiry_data), request->allocationLength);
		return TRUE;
	}
	case SCSI_READ_CAPACITY_10:
	{
		const struct SCSICdbReadCapacity10 *request = (const struct SCSICdbReadCapacity10 *)cdb;
		BOOL pmi = (request->pmi & SCSI_READ_CAPACITY_PMI) != 0;
		ULONG last;
		ULONG sense = capacity_check(view, pmi, request->lba, &last);
		if (sense != VD_SENSE_NONE)
			return vd_scsi_fail(unit, io, scsi, sense);
		const struct SCSICapacity10 data = {
			.lastBlock = last,
			.blockLength = 1UL << shift,
		};
		response_copy(scsi, &data, sizeof(data), sizeof(data));
		return TRUE;
	}
	case SCSI_SERVICE_ACTION_IN_16:
	{
		const struct SCSICdbServiceAction16 *request = (const struct SCSICdbServiceAction16 *)cdb;
		if ((request->serviceAction & SCSI_SERVICE_ACTION) != SCSI_READ_CAPACITY_16)
			break;
		BOOL pmi = (request->flags & SCSI_READ_CAPACITY_PMI) != 0;
		ULONG last;
		ULONG sense = capacity_check(view, pmi, request->lba, &last);
		if (sense != VD_SENSE_NONE)
			return vd_scsi_fail(unit, io, scsi, sense);
		const struct SCSICapacity16 data = {
			.lastBlock = last,
			.blockLength = 1UL << shift,
		};
		response_copy(scsi, &data, sizeof(data), request->allocationLength);
		return TRUE;
	}
	case SCSI_MODE_SENSE_6:
	{
		const struct SCSICdbModeSense6 *request = (const struct SCSICdbModeSense6 *)cdb;
		struct ModeSense6Data data;
		ULONG pages = mode_pages(view, request->page, data.pages);
		if (!pages)
			return vd_scsi_fail(unit, io, scsi, VD_SENSE_BAD_FIELD);
		data.header = (struct SCSIModeHeader6){
			.dataLength = (UBYTE)(sizeof(data.header) - sizeof(data.header.dataLength) + pages),
		};
		response_copy(scsi, &data, sizeof(data.header) + pages, request->allocationLength);
		return TRUE;
	}
	case SCSI_MODE_SENSE_10:
	{
		const struct SCSICdbModeSense10 *request = (const struct SCSICdbModeSense10 *)cdb;
		struct ModeSense10Data data;
		ULONG pages = mode_pages(view, request->page, data.pages);
		if (!pages)
			return vd_scsi_fail(unit, io, scsi, VD_SENSE_BAD_FIELD);
		data.header = (struct SCSIModeHeader10){
			.dataLength = (UWORD)(sizeof(data.header) - sizeof(data.header.dataLength) + pages),
		};
		response_copy(scsi, &data, sizeof(data.header) + pages, request->allocationLength);
		return TRUE;
	}
	case SCSI_SYNCHRONIZE_CACHE_10:
	case SCSI_SYNCHRONIZE_CACHE_16:
		*draft = (struct VDWrappedIO){
			.forwarded = { .io_Command = CMD_UPDATE },
			.client = io,
			.unit = unit,
			.scsi = scsi,
		};
		return FALSE;
	default:
		if (!vd_cdb_range(cdb, cdbLength, &range) || range.kind == VD_CDB_WRITE_SAME)
			break;
		if (range.lba > capacity || range.blocks > capacity - range.lba)
			return vd_scsi_fail(unit, io, scsi, VD_SENSE_BAD_LBA);
		if (range.kind == VD_CDB_VERIFY)
		{
			if (range.flags & SCSI_VERIFY_BYTCHK)
				return vd_scsi_fail(unit, io, scsi, VD_SENSE_BAD_FIELD);  /* no compare here */
			return TRUE;                            /* the range was the check */
		}
		if (!range.blocks)
			return TRUE;
		uint64_t bytes = (uint64_t)range.blocks << shift;
		if (bytes > scsi->scsi_Length || !scsi->scsi_Data)
			return vd_scsi_fail(unit, io, scsi, VD_SENSE_BAD_FIELD);
		uint64_t parentOffset = (view->start + range.lba) << shift;
		*draft = (struct VDWrappedIO){
			.forwarded = {
				.io_Command = range.kind == VD_CDB_WRITE ? NSCMD_TD_WRITE64 : NSCMD_TD_READ64,
				.io_Data = scsi->scsi_Data,
				.io_Length = (ULONG)bytes,
				.io_Offset = (ULONG)parentOffset,
				.io_Actual = (ULONG)(parentOffset >> 32),
			},
			.client = io,
			.unit = unit,
			.scsi = scsi,
		};
		return FALSE;
	}
	return vd_scsi_fail(unit, io, scsi, VD_SENSE_BAD_OPCODE);
}

/**
 * vd_scsi_done() - Complete an emulated HD_SCSICMD from its forwarded request.
 * @unit:      the unit; on an error its REQUEST SENSE data becomes the sense
 * @io:        the client's request; io_Error is written, the caller replies it
 * @scsi:      the command (io_Data): scsi_Actual becomes the forwarded
 *             request's io_Actual
 * @forwarded: the forwarded request, back from the parent
 *
 * Success is SCSI_STATUS_GOOD and io_Error 0, an abort (vd_forward_abort())
 * io_Error IOERR_ABORTED with no sense. An error is CHECK CONDITION and
 * HFERR_BadStatus (vd_scsi_fail()) with DATA PROTECT, write protected
 * (07/27h/00) for TDERR_WriteProt, else MEDIUM ERROR, unrecovered read error
 * (03/11h/00) for a read and write error (03/0Ch/00) for a write or a
 * SYNCHRONIZE CACHE. Runs in the host's task (VDriveComplete()).
 */
void vd_scsi_done(struct VDUnit *unit, struct IOStdReq *io, struct SCSICmd *scsi,
                  const struct IOStdReq *forwarded)
{
	scsi->scsi_Actual = forwarded->io_Actual;
	if (!forwarded->io_Error)
	{
		scsi->scsi_Status = SCSI_STATUS_GOOD;
		io->io_Error = 0;
	}
	else if (forwarded->io_Error == IOERR_ABORTED)
	{
		io->io_Error = IOERR_ABORTED;           /* the client's AbortIO(), passed on */
	}
	else if (forwarded->io_Error == TDERR_WriteProt)
	{
		vd_scsi_fail(unit, io, scsi, VD_SENSE_WRITE_PROTECTED);
	}
	else
	{
		vd_scsi_fail(unit, io, scsi, forwarded->io_Command == NSCMD_TD_READ64 ? VD_SENSE_READ_ERROR
		                                                                        : VD_SENSE_WRITE_ERROR);
	}
}
