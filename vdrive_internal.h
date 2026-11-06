/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * vdrive_internal.h — what the units of the virtual-drive device share; the host's
 * side is vdrive.h.
 *
 * Part of the generic autoboot/automount mounter; see LICENSE for the
 * copyright and licence covering this file.
 *
 * The units, each calling only those listed after it:
 *   vdrive.c          the host's lifecycle calls: rescan a parent unit, take its
 *                     virtual drives offline, destroy the device
 *   vdrive_fence.c    the host's write checks on a parent unit
 *   vdrive_device.c   the exec device: its making, its vectors, the trackdisk
 *                     commands
 *   vdrive_forward.c  the requests sent to the parent unit, and their completion
 *                     (the host's VDriveComplete())
 *   vdrive_scsi.c     the SCSI target behind HD_SCSICMD, and the CDB decoder the
 *                     fence shares
 *   vdrive_unit.c     parent records and units: made, looked up, read, freed
 *
 * Names: VDrive*() and VDRIVE_* are the host's (vdrive.h); SCSI_* and
 * struct SCSI* are the SCSI standards' wire formats (vdrive_scsi.h). vd_*(),
 * struct VD* and VD_* are what the units share, declared here beside the
 * generic helpers (new_list(), new_ignore_port()); what one unit keeps to itself
 * is static and unprefixed. A function's name puts its object first:
 * vd_parent_find(), vd_unit_view(), fence_clear().
 *
 * ROM-clean: all state lives in the device base or in MEMF_PUBLIC allocations,
 * and every table is const.
 *
 * Locking: everything the device shares is guarded by Forbid(), and touched
 * from tasks only. A rescan changes the parents list, a parent's fence, block
 * size and units, and a unit's state; BeginIO and AbortIO() change a unit's
 * held change interrupts; a client's task moves a wrapped request from the free
 * list to the busy list, and the host's puts it back (VDriveComplete()). No
 * interrupt touches any of it, so nothing takes Disable(), which on a PiStorm
 * is a write across the Amiga bus where Forbid() is not. AbortIO() on a virtual
 * unit is task-only for that reason, as it is on both hosts.
 */
#ifndef VDRIVE_INTERNAL_H
#define VDRIVE_INTERNAL_H

#include "mounter_internal.h"
#include "vdrive.h"
#include "vdrive_scsi.h"

#include <exec/devices.h>
#include <exec/interrupts.h>
#include <devices/newstyle.h>

#define VD_SEED_WRAPPED   4     /* wrapped requests allocated with the device */

/* A virtual drive's unit: what io_Unit points at. */
struct VDUnit
{
	struct Unit unit;           /* first: io_Unit points here */
	struct VDriveParent *parent;/* parent device/unit */

	uint64_t start;             /* first block on the parent */
	ULONG length;               /* in parent blocks */
	BOOL online;
	BOOL fenced;                /* its blocks read-only on the parent: while online, and
	                               offline on a medium that cannot be read */
	ULONG changeCount;
	struct MinList changeInts;  /* held TD_ADDCHANGEINT requests */
	ULONG sense;                /* VD_SENSE_*: what REQUEST SENSE returns */
};

/* A host unit that has had a virtual drive, and the host's handle for it
 * (vdrive.h). Never freed before the device. */
struct VDriveParent
{
	struct MinNode node;

	struct VDrive *device;      /* the device it belongs to */
	ULONG unitNumber;           /* on the host device */
	struct IOStdReq openReq;    /* the unit held open: io_Device and io_Unit to forward to */

	UBYTE blockShift;
	BOOL removable;
	BOOL fenced;                /* a unit of it is fenced: FALSE is the fast path */
	struct VDUnit *units[VDRIVE_SLOTS];
};

/* A wrapped request: the forwarded request the parent unit sees, wrapped with
 * what completes the client's request behind it. The client's request itself
 * cannot go: its command (CMD_READ, ETD_WRITE, HD_SCSICMD...), offset (from the
 * unit's start, or a CDB's LBA) and buffer (io_Data, or scsi_Data) are the
 * virtual unit's, not the parent's. A caller drafts one on its stack, and
 * vd_forward() sends a copy from the free list. */
struct VDWrappedIO
{
	struct IOStdReq forwarded;  /* first: the forwarded request (and the free-list node) */
	struct MinNode busyNode;    /* on the device's busy list while in flight */
	struct IOStdReq *client;    /* the client's request; NULL: the device's own (a last-close flush) */
	struct VDUnit *unit;        /* the virtual unit it is for */
	struct SCSICmd *scsi;       /* the HD_SCSICMD being emulated, or NULL */
};

struct VDrive
{
	struct Library lib;         /* first: the device base */
	struct ExecBase *SysBase;
	const char *parentName;		/* parent device name */

	struct MsgPort forwardPort; /* PA_IGNORE: the reply port that marks every forwarded request */
	struct MsgPort openPort;    /* PA_IGNORE: the parents' openReq, never replied */

	struct MinList freeWrapped; /* Pool of free wrapped requests */
	struct MinList busyWrapped; /* wrapped requests in flight: sent and not back yet */

	struct MinList parents;		/* list of parent units */
	ULONG fencedParents;        /* parents with a unit fenced, for VDriveAnyFenced() */
};

/* What a rescan may change under a request, copied at once (vd_unit_view()). */
struct VDView
{
	BOOL online;
	uint64_t start;             /* first block on the parent */
	ULONG length;               /* in parent blocks */
	ULONG changeCount;
	UBYTE blockShift;           /* log2 of the parent's block size */
	BOOL removable;             /* the parent's medium is */
};

/**
 * new_list() - Initialise a list header as empty, as NewList() does.
 * @list: the list header; a struct List passed as one keeps its lh_Type
 */
static inline void new_list(struct MinList *list)
{
	list->mlh_Head = (struct MinNode *)&list->mlh_Tail;
	list->mlh_Tail = NULL;
	list->mlh_TailPred = (struct MinNode *)&list->mlh_Head;
}

/**
 * list_empty() - Tell whether a list is empty, as IsListEmpty() does.
 * @l: the list header
 *
 * Return: TRUE when it holds no node
 */
static inline BOOL list_empty(const struct MinList *l)
{
	return l->mlh_TailPred == (const struct MinNode *)&l->mlh_Head;
}

/**
 * new_ignore_port() - Initialise a message port that signals nobody.
 * @port: the port, zeroed: NT_MSGPORT, PA_IGNORE, an empty message list
 *
 * A message that reaches it is queued there and nothing more.
 */
static inline void new_ignore_port(struct MsgPort *port)
{
	port->mp_Node.ln_Type = NT_MSGPORT;
	port->mp_Flags = PA_IGNORE;
	new_list((struct MinList *)&port->mp_MsgList);
}

/**
 * vd_reply() - Complete a client's request.
 * @device: the device base (for SysBase)
 * @io:     the request; ReplyMsg()ed unless IOF_QUICK is set, when the caller
 *          of BeginIO() finds it done on return. The caller sets io_Actual
 * @error:  its io_Error
 */
static inline void vd_reply(struct VDrive *device, struct IOStdReq *io, BYTE error)
{
	struct ExecBase *SysBase = device->SysBase;
	io->io_Error = error;
	if (!(io->io_Flags & IOF_QUICK))
		ReplyMsg(&io->io_Message);
}

/* ---- vdrive_device.c: the exec device ------------------------------------- */

struct VDrive *vd_create(const struct VDriveConfig *config);
void vd_free(struct VDrive *device);

/* ---- vdrive_forward.c: to the parent unit and back ------------------------- */

void vd_forward_init(struct VDrive *device);
void vd_forward_free(struct VDrive *device);
void vd_forward(struct VDrive *device, const struct VDWrappedIO *draft);
LONG vd_forward_abort(struct VDrive *device, struct IOStdReq *client);

/* ---- vdrive_scsi.c: the SCSI target ---------------------------------------- */

/* Sense data in one ULONG: the sense key above SCSI_ASC_*'s ASC and ASCQ. */
#define VD_SENSE(key, asc)        (((ULONG)(key) << 16) | (asc))
#define VD_SENSE_KEY(sense)       ((UBYTE)((sense) >> 16))
#define VD_SENSE_ASC(sense)       ((UBYTE)((sense) >> 8))
#define VD_SENSE_ASCQ(sense)      ((UBYTE)(sense))

#define VD_SENSE_NONE             VD_SENSE(SCSI_KEY_NO_SENSE, SCSI_ASC_NONE)
#define VD_SENSE_NOT_PRESENT      VD_SENSE(SCSI_KEY_NOT_READY, SCSI_ASC_MEDIUM_NOT_PRESENT)
#define VD_SENSE_WRITE_ERROR      VD_SENSE(SCSI_KEY_MEDIUM_ERROR, SCSI_ASC_WRITE_ERROR)
#define VD_SENSE_READ_ERROR       VD_SENSE(SCSI_KEY_MEDIUM_ERROR, SCSI_ASC_UNRECOVERED_READ)
#define VD_SENSE_BAD_OPCODE       VD_SENSE(SCSI_KEY_ILLEGAL_REQUEST, SCSI_ASC_INVALID_OPCODE)
#define VD_SENSE_BAD_LBA          VD_SENSE(SCSI_KEY_ILLEGAL_REQUEST, SCSI_ASC_LBA_OUT_OF_RANGE)
#define VD_SENSE_BAD_FIELD        VD_SENSE(SCSI_KEY_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB)
#define VD_SENSE_WRITE_PROTECTED  VD_SENSE(SCSI_KEY_DATA_PROTECT, SCSI_ASC_WRITE_PROTECTED)
#define VD_SENSE_NO_RESOURCES     VD_SENSE(SCSI_KEY_ABORTED_COMMAND, SCSI_ASC_NO_RESOURCES)

/* What a CDB that vd_cdb_range() decodes does with its blocks. */
enum
{
	VD_CDB_NONE,                /* it names no block range */
	VD_CDB_READ,
	VD_CDB_VERIFY,              /* moves no blocks */
	VD_CDB_WRITE,               /* WRITE, WRITE AND VERIFY */
	VD_CDB_WRITE_SAME,
};

/* The blocks a READ, WRITE, VERIFY or WRITE SAME CDB names (vd_cdb_range()). */
struct VDCdbRange
{
	uint64_t lba;
	ULONG blocks;               /* 0 with WRITE SAME: to the end of the medium */
	UBYTE kind;                 /* VD_CDB_* */
	UBYTE flags;                /* the CDB's flags byte (SCSI_VERIFY_BYTCHK); 0 for a 6-byte one */
};

BOOL vd_cdb_range(const UBYTE *cdb, UWORD length, struct VDCdbRange *range);
void vd_scsi_check(struct SCSICmd *scsi, ULONG sense);
BOOL vd_scsi_fail(struct VDUnit *unit, struct IOStdReq *io, struct SCSICmd *scsi, ULONG sense);
BOOL vd_scsi_command(struct VDUnit *unit, struct IOStdReq *io, const struct VDView *view,
                     struct VDWrappedIO *draft);
void vd_scsi_done(struct VDUnit *unit, struct IOStdReq *io, struct SCSICmd *scsi,
                  const struct IOStdReq *forwarded);

/* ---- vdrive_unit.c: parent records and units -------------------------------- */

struct VDriveParent *vd_parent_find(struct VDrive *device, ULONG parentUnit);
struct VDriveParent *vd_parent_lookup(struct VDrive *device, ULONG parentUnit);
struct VDriveParent *vd_parent_get(struct VDrive *device, ULONG parentUnit);
struct VDUnit *vd_unit_new(struct VDrive *device, struct VDriveParent *parent);
void vd_parents_free(struct VDrive *device);
void vd_unit_view(struct VDrive *device, const struct VDUnit *unit, struct VDView *view);
void vd_unit_geometry(const struct VDView *view, struct DriveGeometry *geometry);

#endif
