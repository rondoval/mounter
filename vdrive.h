/*
 * vdrive.h — virtual drives as units of a device of their own.
 *
 * A virtual drive is a partition holding a whole RDB disk: MBR type 0x76 (primary
 * or logical) or Emu68's GPT type 3F82EEBC-87C9-4097-8165-89D6540557C0. A host
 * block driver keeps one virtual device, and each virtual drive on one of its
 * units becomes a unit there, addressed from the partition's first block. The
 * inner RDB then mounts, boots and partitions exactly as on Emu68's SD card, and
 * filesystems that record their own position (SFS) see the same one everywhere.
 *
 * The device forwards I/O to the parent unit as 64-bit trackdisk requests. On the
 * parent, the blocks of every virtual drive that has a unit are read-only to
 * everyone else, so that no two paths write them: the host asks
 * VDriveWriteAllowed()/VDriveScsiAllowed() on its write paths.
 */
#ifndef VDRIVE_H
#define VDRIVE_H

#include <exec/types.h>
#include <exec/io.h>
#include <devices/scsidisk.h>
#include <stdint.h>

/* Unit numbers: ten slots per parent unit, slot = order among its virtual
 * drives; a virtual drive past them gets no unit and stays an ordinary partition
 * of the parent. HDToolBox reads unit = address + 10 * LUN, so it shows slot as
 * the address and the parent unit as the LUN. */
#define VDRIVE_SLOTS              10
#define VDRIVE_UNIT(parent, slot) ((parent) * VDRIVE_SLOTS + (slot))
#define VDRIVE_PARENT(unit)       ((unit) / VDRIVE_SLOTS)
#define VDRIVE_SLOT(unit)         ((unit) % VDRIVE_SLOTS)

struct VDrive;          /* the virtual device; opaque to hosts */
struct VDriveParent;    /* a parent unit's record on it, the host's handle for the unit;
                           opaque, valid until VDriveDestroy() */

struct VDriveConfig
{
	struct ExecBase *SysBase;
	const char *name;           /* the virtual device, e.g. "nvmevd.device" */
	const char *idString;       /* its lib_IdString, without "$VER: " */
	UWORD version, revision;    /* its lib_Version and lib_Revision: the host's */
	const char *parentName;     /* the host device, e.g. "nvme.device" */
};

/* The API; each function's contract is its kernel-doc: in vdrive.c for the
 * lifecycle, vdrive_fence.c for the write checks, vdrive_forward.c for
 * VDriveComplete(). A host makes the lifecycle calls that do I/O,
 * VDriveRefresh() and VDriveDestroy(), from one task at a time. It keeps the
 * parent handle VDriveRefresh() gives with its own unit, and passes it to the
 * calls about that unit; NULL, as before a unit's first rescan, means no
 * virtual drives. */

/* Rescan a parent unit's virtual drives, creating the device on the first one. */
ULONG VDriveRefresh(struct VDrive **devicePtr, const struct VDriveConfig *config, ULONG parentUnit,
                    struct VDriveParent **parentPtr);

/* Take a parent unit's virtual drives offline: its medium or the unit is gone. */
void VDriveParentGone(struct VDriveParent *parent);

/* Count the opens of the host device the virtual device holds. */
ULONG VDriveHeldOpens(struct VDrive *device);

/* Remove and free the device. */
BOOL VDriveDestroy(struct VDrive *device);

/* Check a write on a parent unit against its virtual drives. */
BOOL VDriveWriteAllowed(struct VDriveParent *parent, const struct IORequest *io,
                        uint64_t offset, uint64_t length);

/* Check an HD_SCSICMD on a parent unit against its virtual drives. */
BOOL VDriveScsiAllowed(struct VDriveParent *parent, const struct IORequest *io,
                       struct SCSICmd *scsi);

/* Tell whether any parent unit has virtual drives. */
BOOL VDriveAnyFenced(struct VDrive *device);

/* Complete a finished request if it is the virtual device's own; the host
 * ReplyMsg()s it only on FALSE. From a task, on every reply path. */
BOOL VDriveComplete(struct VDrive *device, struct IORequest *io);

#endif
