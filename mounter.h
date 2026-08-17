#ifndef MOUNTER_H
#define MOUNTER_H

// How to mount one filesystem family found on non-RDB media (MBR/GPT
// partitions, superfloppies, data CDs). All strings are C strings owned by
// the caller for the duration of MountDrive().
//
// The caller allocates this, and the mounter reads every field, so it must be
// zero-filled. There is no mixed-version compatibility: a caller compiled against an older,
// shorter MountFS hands the mounter whatever lies past the end of its object.
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
	// MOUNTFS_* flags. 0: classic behavior.
	ULONG fsFlags;
};

// MountFS flags.
#define MOUNTFS_FORCELOAD      0x0001  // prefer 'handler' over a FileSystem.resource entry
                                       // with the same dostype (mountlist ForceLoad=1)

// MountStruct flags.
#define MSF_NO_RDB             0x0001  // skip RDB scanning
#define MSF_NO_LEGACY          0x0002  // skip MBR/GPT/superfloppy scanning
#define MSF_NO_CD              0x0004  // skip CD mounting (data discs, audio discs and RDB-CD)
#define MSF_LEGACY_FIRST_ONLY  0x0008  // mount only the first MBR/GPT/superfloppy filesystem per unit
#define MSF_NO_BOOT            0x0010  // never create pre-DOS boot nodes, mount non-bootable
#define MSF_CD_AUDIO           0x0020  // cdFS handler understands audio-only discs; mount them via cdFS (non-bootable)
#define MSF_CD_ANYFMT          0x0040  // cdFS handler identifies disc formats itself (High Sierra,
                                       // UDF, HFS/HFS+ as well as ISO9660); mount a data disc that
                                       // has no ISO9660 PVD instead of rejecting it

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
	// Recipe for data CDs (and, with MSF_CD_AUDIO, audio-only discs). What
	// format the disc actually carries is the handler's business; see
	// MSF_CD_ANYFMT. NULL: classic behavior (CD01/CDVD from
	// FileSystem.resource only, ISO9660 discs only).
	const struct MountFS *cdFS;
	// Recommended DMA buffer alignment in bytes (a power of two).
	// Nonzero: recipe-mounted filesystems get their buffers in
	// MEMF_FAST | MEMF_PUBLIC (de_BufMemType) with de_Mask enforcing
	// this alignment.
	// 0: classic behavior (de_BufMemType MEMF_ANY, de_Mask word-aligned).
	ULONG dmaAlign;
	// Recipe for exFAT partitions/superfloppies. NULL: exFAT is skipped.
	const struct MountFS *exfatFS;
};

APTR W_CreateIORequest(struct MsgPort *ioReplyPort, ULONG size, struct ExecBase *SysBase);
void W_DeleteIORequest(APTR iorequest, struct ExecBase *SysBase);
struct MsgPort *W_CreateMsgPort(struct ExecBase *SysBase);
void W_DeleteMsgPort(struct MsgPort *port, struct ExecBase *SysBase);

LONG MountDrive(struct MountStruct *ms);

#endif
