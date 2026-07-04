#ifndef MOUNTER_H
#define MOUNTER_H

// How to mount one filesystem family found on non-RDB media (MBR/GPT
// partitions, superfloppies, data CDs). All strings are C strings owned by
// the caller for the duration of MountDrive().
struct MountFS
{
	// de_DosType and the FileSystem.resource lookup key.
	ULONG dosType;
	// Handler file, e.g. "L:fat95". Used as dn_Handler (with dn_GlobalVec=-1
	// and dn_StackSize below) when dosType is not in FileSystem.resource, so
	// DOS loads it on first access. NULL: FileSystem.resource only.
	const UBYTE *handler;
	// Preferred DOS device name; a trailing digit is ensured ("UMSD" ->
	// "UMSD0", "UMSD0" stays) and bumped past collisions ("UMSD1", ...,
	// "UMSD10"). NULL: "MS0" (partitions/superfloppy) or "CD0" (CDs).
	const UBYTE *dosName;
	// de_Control string (handler-specific options). NULL: none.
	const UBYTE *control;
	// de_NumBuffers. 0: 5.
	ULONG buffers;
	// de_MaxTransfer. 0: 0x100000.
	ULONG maxTransfer;
	// dn_StackSize when the handler comes from 'handler'. 0: 8192.
	ULONG stackSize;
};

// MountStruct flags.
#define MSF_NO_RDB             0x0001  // skip RDB scanning
#define MSF_NO_LEGACY          0x0002  // skip MBR/GPT/superfloppy scanning
#define MSF_NO_CD              0x0004  // skip CD mounting (ISO9660 and RDB-CD)
#define MSF_LEGACY_FIRST_ONLY  0x0008  // mount only the first MBR/GPT/superfloppy filesystem per unit
#define MSF_NO_BOOT            0x0010  // never create pre-DOS boot nodes, mount non-bootable

struct MountStruct
{
	// Device name. ("myhddriver.device")
	// Offset 0.
	const UBYTE *deviceName;
	// Unit number pointer or single integer value.
	// if >= 0x100 (256), pointer to array of ULONGs, first ULONG is number of unit numbers followed (for example { 2, 0, 1 }. 2 units, unit numbers 0 and 1).
	// if < 0x100 (256): used as a single unit number value.
	// Offset 4.
	ULONG *unitNum;
	// Name string used to set Creator field in FileSystem.resource (if KS 1.3) and in FileSystem.resource entries.
	// If NULL: use device name.
	// Offset 8.
	const UBYTE *creatorName;
	// ConfigDev: set if autoconfig board autoboot support is wanted.
	// If NULL and bootable partition found: fake ConfigDev is automatically created.
	// Offset 12.
	struct ConfigDev *configDev;
	// SysBase.
	// Offset 16.
	struct ExecBase *SysBase;
	// LUNs
	// Offset 20.
	BOOL luns;
	// Short/Long Spinup
	// Offset 22.
	BOOL slowSpinup;
	// Ignore RDBFF_LAST flag
	BOOL ignoreLast;
	// Host controller SCSI ID - set to 255 for non-SCSI controllers
	UBYTE hostId;
	UBYTE pad;
	// Everything below is optional; zero-fill for the classic behavior.
	// MSF_* flags.
	ULONG flags;
	// Recipe for FAT partitions/superfloppies. NULL: classic behavior
	// (dostype 0x46415401 from FileSystem.resource only).
	const struct MountFS *fatFS;
	// Recipe for NTFS partitions/superfloppies. NULL: NTFS is skipped.
	const struct MountFS *ntfsFS;
	// Recipe for ISO9660 data CDs. NULL: classic behavior (CD01/CDVD from
	// FileSystem.resource only).
	const struct MountFS *cdFS;
};

APTR W_CreateIORequest(struct MsgPort *ioReplyPort, ULONG size, struct ExecBase *SysBase);
void W_DeleteIORequest(APTR iorequest, struct ExecBase *SysBase);
struct MsgPort *W_CreateMsgPort(struct ExecBase *SysBase);
void W_DeleteMsgPort(struct MsgPort *port, struct ExecBase *SysBase);

LONG MountDrive(struct MountStruct *ms);

#endif
