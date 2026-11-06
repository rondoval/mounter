/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * vdrive_device.c — the virtual-drive device itself: its making, its vectors,
 * and the trackdisk commands, answered here or sent on to the parent unit
 * (vdrive_forward.c). HD_SCSICMD is vdrive_scsi.c's.
 *
 * Part of the generic autoboot/automount mounter; see LICENSE for the
 * copyright and licence covering this file.
 */
#include "vdrive_internal.h"

/**
 * dev_open() - The device's Open vector: open a virtual drive's unit.
 * @io:         the request (A1): io_Error always; io_Unit and ln_Type
 *              (NT_REPLYMSG) on success
 * @unitNumber: the unit (D0): VDRIVE_UNIT(parent, slot)
 * @flags:      the OpenDevice() flags (D1), unused
 * @device:     the device base (A6)
 *
 * Runs under OpenDevice()'s Forbid(), which covers vd_parent_find(). Any unit a
 * rescan ever created opens, online or not. On success io_Error is 0, the
 * unit's and the device's open counts go up and LIBF_DELEXP is cleared; else
 * io_Error is TDERR_BadUnitNum and nothing else changes. OpenDevice() takes the
 * result from io_Error.
 */
static void dev_open(struct IOStdReq *io asm("a1"), ULONG unitNumber asm("d0"),
                     ULONG flags asm("d1"), struct VDrive *device asm("a6"))
{
	(void)flags;
	struct VDriveParent *parent = vd_parent_find(device, VDRIVE_PARENT(unitNumber));
	struct VDUnit *unit = parent ? parent->units[VDRIVE_SLOT(unitNumber)] : NULL;
	if (!unit)
	{
		io->io_Error = TDERR_BadUnitNum;
		return;
	}
	unit->unit.unit_OpenCnt++;
	device->lib.lib_OpenCnt++;
	device->lib.lib_Flags &= (UBYTE)~LIBF_DELEXP;
	io->io_Unit = &unit->unit;
	io->io_Error = 0;
	io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
}

/**
 * dev_close() - The device's Close vector: close a virtual drive's unit.
 * @io:     the request (A1); io_Unit and io_Device become -1
 * @device: the device base (A6)
 *
 * Runs under CloseDevice()'s Forbid(). The parent stays open while the device
 * exists, so its own last-close flush never runs: flush on behalf of the unit's
 * last client instead. That CMD_UPDATE goes to the parent with no client
 * request to reply (skipped while the unit is offline or the pool is empty and
 * memory short), and it is on device->busyWrapped before lib_OpenCnt drops, so
 * VDriveDestroy() cannot free the device under it.
 *
 * Return: 0, no seglist: the device never expunges itself on a close,
 * LIBF_DELEXP or not; the host frees it with VDriveDestroy()
 */
static BPTR dev_close(struct IOStdReq *io asm("a1"), struct VDrive *device asm("a6"))
{
	struct VDUnit *unit = (struct VDUnit *)io->io_Unit;
	io->io_Unit = (struct Unit *)-1;
	io->io_Device = (struct Device *)-1;
	if (unit && --unit->unit.unit_OpenCnt == 0 && unit->online)
	{
		const struct VDWrappedIO flush = {
			.forwarded = { .io_Command = CMD_UPDATE },
			.unit = unit,
		};
		vd_forward(device, &flush);
	}
	device->lib.lib_OpenCnt--;
	return 0;
}

/**
 * dev_expunge() - The device's Expunge vector: set LIBF_DELEXP, nothing more.
 * @device: the device base (A6)
 *
 * The host removes and frees the device with VDriveDestroy(), which also closes
 * the parent units it holds open.
 *
 * Return: 0, no seglist: the device stays on the device list and in memory
 */
static BPTR dev_expunge(struct VDrive *device asm("a6"))
{
	device->lib.lib_Flags |= LIBF_DELEXP;
	return 0;
}

/**
 * dev_null() - The device's reserved vector.
 *
 * Return: 0, always
 */
static ULONG dev_null(void)
{
	return 0;
}

/**
 * block_transfer() - Check a block transfer and send it to the parent.
 * @device:        the device base
 * @unit:          the unit
 * @io:            the request
 * @view:          the unit's view
 * @parentCommand: what it becomes on the parent: NSCMD_TD_READ64,
 *                 NSCMD_TD_WRITE64 or NSCMD_TD_FORMAT64
 * @wide:          a 64-bit command, with the offset's high half in io_Actual
 *
 * Fails with TDERR_DiskChanged while offline; else io_Actual is 0 and the
 * request fails with IOERR_BADLENGTH for an offset or length that is not whole
 * blocks, or IOERR_BADADDRESS past the unit's end. One of length 0 succeeds at
 * once; the rest goes to the parent from the unit's start (vd_forward()).
 */
static void block_transfer(struct VDrive *device, struct VDUnit *unit, struct IOStdReq *io,
                           const struct VDView *view, UWORD parentCommand, BOOL wide)
{
	if (!view->online)
	{
		vd_reply(device, io, TDERR_DiskChanged);
		return;
	}

	uint64_t offset = io->io_Offset;
	if (wide)
		offset |= (uint64_t)io->io_Actual << 32;

	const UBYTE shift = view->blockShift;
	uint64_t size = (uint64_t)view->length << shift;
	ULONG length = io->io_Length;
	io->io_Actual = 0;

	if ((offset | length) & ((1UL << shift) - 1))
	{
		vd_reply(device, io, IOERR_BADLENGTH);
		return;
	}

	if (offset > size || length > size - offset)
	{
		vd_reply(device, io, IOERR_BADADDRESS);
		return;
	}

	if (!length)
	{
		vd_reply(device, io, 0);
		return;
	}
	
	uint64_t parentOffset = (view->start << shift) + offset;
	const struct VDWrappedIO draft = {
		.forwarded = {
			.io_Command = parentCommand,
			.io_Data = io->io_Data,
			.io_Length = length,
			.io_Offset = (ULONG)parentOffset,
			.io_Actual = (ULONG)(parentOffset >> 32),
		},
		.client = io,
		.unit = unit,
	};
	vd_forward(device, &draft);
}

/**
 * changeint_remove() - Take a request off a unit's held change interrupts.
 * @device: the device base (for SysBase)
 * @unit:   the unit
 * @io:     the request
 *
 * Return: TRUE if @io was held and is removed; FALSE if it was not held
 */
static BOOL changeint_remove(struct VDrive *device, struct VDUnit *unit, struct IOStdReq *io)
{
	struct ExecBase *SysBase = device->SysBase;
	BOOL held = FALSE;
	Forbid();
	for (struct MinNode *node = unit->changeInts.mlh_Head; node->mln_Succ; node = node->mln_Succ)
	{
		if (node == (struct MinNode *)&io->io_Message.mn_Node)
		{
			Remove(&io->io_Message.mn_Node);
			held = TRUE;
			break;
		}
	}
	Permit();
	return held;
}

/* What NSCMD_DEVICEQUERY lists as supported; 0 ends it. */
static const UWORD nsd_commands[] = {
	CMD_RESET,
	CMD_READ,
	CMD_WRITE,
	CMD_UPDATE,
	CMD_CLEAR,
	CMD_STOP,
	CMD_START,
	CMD_FLUSH,
	TD_MOTOR,
	TD_SEEK,
	TD_FORMAT,
	TD_CHANGENUM,
	TD_CHANGESTATE,
	TD_PROTSTATUS,
	TD_ADDCHANGEINT,
	TD_REMCHANGEINT,
	TD_GETGEOMETRY,
	HD_SCSICMD,
	TD_READ64,
	TD_WRITE64,
	TD_SEEK64,
	TD_FORMAT64,
	ETD_READ,
	ETD_WRITE,
	ETD_UPDATE,
	ETD_CLEAR,
	ETD_MOTOR,
	ETD_SEEK,
	ETD_FORMAT,
	NSCMD_DEVICEQUERY,
	NSCMD_TD_READ64,
	NSCMD_TD_WRITE64,
	NSCMD_TD_SEEK64,
	NSCMD_TD_FORMAT64,
	0
};

/**
 * device_query() - Answer NSCMD_DEVICEQUERY.
 * @device: the device base
 * @io:     the request: io_Data a struct NSDeviceQueryResult of io_Length bytes
 *
 * A trackdisk-like device with nsd_commands[]; io_Actual is the result's size.
 * Too short a buffer, or none, is IOERR_BADLENGTH.
 */
static void device_query(struct VDrive *device, struct IOStdReq *io)
{
	struct NSDeviceQueryResult *result = (struct NSDeviceQueryResult *)io->io_Data;
	if (!result || io->io_Length < sizeof(*result))
	{
		vd_reply(device, io, IOERR_BADLENGTH);
		return;
	}
	result->nsdqr_DevQueryFormat = 0;
	result->nsdqr_SizeAvailable = sizeof(*result);
	result->nsdqr_DeviceType = NSDEVTYPE_TRACKDISK;
	result->nsdqr_DeviceSubType = 0;
	result->nsdqr_SupportedCommands = (APTR)nsd_commands;
	io->io_Actual = sizeof(*result);
	vd_reply(device, io, 0);
}

/**
 * command_is_etd() - Tell whether a command is an ETD_ one, in an IOExtTD.
 * @command: io_Command
 *
 * TDF_EXTCOM alone cannot tell: the NSCMD_TD_*64 commands have that bit set as
 * well, and come in a plain IOStdReq with no iotd_Count to read.
 *
 * Return: TRUE for the ETD_ commands trackdisk.h defines; FALSE otherwise
 */
static BOOL command_is_etd(UWORD command)
{
	switch (command)
	{
	case ETD_READ:
	case ETD_WRITE:
	case ETD_UPDATE:
	case ETD_CLEAR:
	case ETD_MOTOR:
	case ETD_SEEK:
	case ETD_FORMAT:
	case ETD_RAWREAD:
	case ETD_RAWWRITE:
		return TRUE;
	default:
		return FALSE;
	}
}

/**
 * dev_beginio() - The device's BeginIO vector: answer or forward a request.
 * @io:     the request (A1) on a unit from dev_open(): an IOStdReq, or an
 *          IOExtTD for an ETD_ command (iotd_Count is read)
 * @device: the device base (A6)
 *
 * Task context: a forwarded command may allocate its wrapped request. One
 * view of the unit (vd_unit_view()) is taken under Forbid(), since a rescan may
 * be moving it, and everything below works from it. An ETD_ command fails with
 * TDERR_DiskChanged when iotd_Count is below the change count.
 *
 * Sent to the parent (vd_forward()), IOF_QUICK cleared, and completed with the
 * parent's io_Error and io_Actual: the block transfers (block_transfer()), and
 * CMD_UPDATE, ETD_UPDATE and TD_PROTSTATUS while online; the HD_SCSICMD
 * transfers and SYNCHRONIZE CACHE (vd_scsi_command()).
 *
 * Answered here: TD_CHANGENUM and TD_CHANGESTATE in io_Actual; TD_GETGEOMETRY
 * (filled in even while offline, which then fails with TDERR_DiskChanged);
 * TD_REMCHANGEINT; NSCMD_DEVICEQUERY (device_query()); CMD_UPDATE, ETD_UPDATE
 * and TD_PROTSTATUS while offline, with success and io_Actual 0; the rest of
 * HD_SCSICMD. CMD_CLEAR, the motor and seek commands, CMD_START, CMD_STOP,
 * CMD_RESET and CMD_FLUSH do nothing and succeed; a short TD_GETGEOMETRY or
 * HD_SCSICMD buffer is IOERR_BADLENGTH; anything else is IOERR_NOCMD.
 * TD_ADDCHANGEINT is held, IOF_QUICK cleared and not replied, until
 * TD_REMCHANGEINT or AbortIO() takes it back.
 */
static void dev_beginio(struct IOStdReq *io asm("a1"), struct VDrive *device asm("a6"))
{
	struct ExecBase *SysBase = device->SysBase;
	struct VDUnit *unit = (struct VDUnit *)io->io_Unit;
	UWORD command = io->io_Command;

	io->io_Message.mn_Node.ln_Type = NT_MESSAGE;
	io->io_Error = 0;

	struct VDView view;
	vd_unit_view(device, unit, &view);

	if (command_is_etd(command) && ((struct IOExtTD *)io)->iotd_Count < view.changeCount)
	{
		vd_reply(device, io, TDERR_DiskChanged);
		return;
	}

	switch (command)
	{
	case CMD_READ:
	case ETD_READ:
		block_transfer(device, unit, io, &view, NSCMD_TD_READ64, FALSE);
		return;
	case TD_READ64:
	case NSCMD_TD_READ64:
		block_transfer(device, unit, io, &view, NSCMD_TD_READ64, TRUE);
		return;
	case CMD_WRITE:
	case ETD_WRITE:
		block_transfer(device, unit, io, &view, NSCMD_TD_WRITE64, FALSE);
		return;
	case TD_WRITE64:
	case NSCMD_TD_WRITE64:
		block_transfer(device, unit, io, &view, NSCMD_TD_WRITE64, TRUE);
		return;
	case TD_FORMAT:
	case ETD_FORMAT:
		block_transfer(device, unit, io, &view, NSCMD_TD_FORMAT64, FALSE);
		return;
	case TD_FORMAT64:
	case NSCMD_TD_FORMAT64:
		block_transfer(device, unit, io, &view, NSCMD_TD_FORMAT64, TRUE);
		return;
	case CMD_UPDATE:
	case ETD_UPDATE:
	case TD_PROTSTATUS:
		if (view.online)
		{
			const struct VDWrappedIO draft = {
				.forwarded = { .io_Command = command == TD_PROTSTATUS ? TD_PROTSTATUS : CMD_UPDATE },
				.client = io,
				.unit = unit,
			};
			vd_forward(device, &draft);
		}
		else
		{
			io->io_Actual = 0;
			vd_reply(device, io, 0);
		}
		return;
	case TD_CHANGENUM:
		io->io_Actual = view.changeCount;
		vd_reply(device, io, 0);
		return;
	case TD_CHANGESTATE:
		io->io_Actual = view.online ? 0 : 1;           /* nonzero: no disk */
		vd_reply(device, io, 0);
		return;
	case TD_GETGEOMETRY:
		if (io->io_Length < sizeof(struct DriveGeometry) || !io->io_Data)
		{
			vd_reply(device, io, IOERR_BADLENGTH);
			return;
		}
		vd_unit_geometry(&view, (struct DriveGeometry *)io->io_Data);
		io->io_Actual = sizeof(struct DriveGeometry);
		vd_reply(device, io, view.online ? 0 : TDERR_DiskChanged);
		return;
	case TD_ADDCHANGEINT:
		io->io_Flags &= (UBYTE)~IOF_QUICK;               /* held until TD_REMCHANGEINT */
		Forbid();
		AddTail((struct List *)&unit->changeInts, &io->io_Message.mn_Node);
		Permit();
		return;
	case TD_REMCHANGEINT:
		changeint_remove(device, unit, io);
		vd_reply(device, io, 0);
		return;
	case NSCMD_DEVICEQUERY:
		device_query(device, io);
		return;
	case HD_SCSICMD:
	{
		if (io->io_Length < sizeof(struct SCSICmd) || !io->io_Data)
		{
			vd_reply(device, io, IOERR_BADLENGTH);
			return;
		}
		struct VDWrappedIO draft;
		if (vd_scsi_command(unit, io, &view, &draft))
			vd_reply(device, io, io->io_Error);
		else
			vd_forward(device, &draft);
		return;
	}
	case CMD_CLEAR:
	case ETD_CLEAR:
	case TD_MOTOR:
	case ETD_MOTOR:
	case TD_SEEK:
	case ETD_SEEK:
	case TD_SEEK64:
	case NSCMD_TD_SEEK64:
	case CMD_START:
	case CMD_STOP:
	case CMD_RESET:
	case CMD_FLUSH:
		io->io_Actual = 0;                          /* the parent's motor and queue are not ours */
		vd_reply(device, io, 0);
		return;
	default:
		vd_reply(device, io, IOERR_NOCMD);
		return;
	}
}

/**
 * dev_abortio() - The device's AbortIO vector: abort here, or pass it on.
 * @io:     the request to abort (A1)
 * @device: the device base (A6)
 *
 * A held TD_ADDCHANGEINT is removed and ReplyMsg()ed with IOERR_ABORTED. A
 * request forwarded to the parent has the abort passed on (vd_forward_abort()):
 * the parent decides, and the request comes back with IOERR_ABORTED or with
 * its result. Anything else is done already, answered here at once. Task
 * context only, as on both hosts: the lists are guarded by Forbid().
 *
 * Return: 0 when @io was a held TD_ADDCHANGEINT or the abort went to the
 * parent; -1 otherwise, @io untouched
 */
static LONG dev_abortio(struct IOStdReq *io asm("a1"), struct VDrive *device asm("a6"))
{
	struct ExecBase *SysBase = device->SysBase;
	if (!changeint_remove(device, (struct VDUnit *)io->io_Unit, io))
		return vd_forward_abort(device, io);
	io->io_Error = IOERR_ABORTED;
	ReplyMsg(&io->io_Message);
	return 0;
}

/* The device's vectors in exec's order, for MakeLibrary(); -1 ends them. */
static const APTR vectors[] = {
	(APTR)dev_open,
	(APTR)dev_close,
	(APTR)dev_expunge,
	(APTR)dev_null,
	(APTR)dev_beginio,
	(APTR)dev_abortio,
	(APTR)-1
};

/**
 * vd_create() - Make the virtual device and add it to the system.
 * @config: the host's configuration; name, idString and parentName are kept by
 *          pointer and must outlive the device
 *
 * MakeLibrary() with the vectors, then the PA_IGNORE port of the held parent
 * opens, the forward port and the free list (vd_forward_init()), and AddDevice()
 * under the same Forbid() as the name check; a lost race frees it all again.
 * Task context.
 *
 * Return: the device; NULL when a device called config->name exists already
 * (logged) or MakeLibrary() fails
 */
struct VDrive *vd_create(const struct VDriveConfig *config)
{
	struct ExecBase *SysBase = config->SysBase;

	struct VDrive *device = (struct VDrive *)MakeLibrary((APTR)vectors, NULL, NULL,
	                                                 sizeof(struct VDrive), 0);
	if (!device)
		return NULL;
	device->lib.lib_Node.ln_Type = NT_DEVICE;
	device->lib.lib_Node.ln_Name = (char *)config->name;
	device->lib.lib_IdString = (APTR)config->idString;
	device->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
	device->lib.lib_Version = config->version;
	device->lib.lib_Revision = config->revision;
	device->SysBase = SysBase;
	device->parentName = config->parentName;
	new_list(&device->parents);
	new_ignore_port(&device->openPort);
	vd_forward_init(device);

	/* The name check and AddDevice() in one Forbid(), so no other device of the
	 * name can slip in between. */
	Forbid();
	BOOL taken = FindName(&SysBase->DeviceList, (CONST_STRPTR)config->name) != NULL;
	if (!taken)
		AddDevice((struct Device *)device);
	Permit();
	if (taken)
	{
		mnt_log("%s already exists\n", config->name);
		vd_free(device);
		return NULL;
	}
	return device;
}

/**
 * vd_free() - Free the device base and its wrapped requests.
 * @device: the device, off the device list (or never on it), with no parent
 *          record left and no wrapped request in flight
 */
void vd_free(struct VDrive *device)
{
	struct ExecBase *SysBase = device->SysBase;
	vd_forward_free(device);
	FreeMem((UBYTE *)device - device->lib.lib_NegSize,
	        (ULONG)device->lib.lib_NegSize + device->lib.lib_PosSize);
}
