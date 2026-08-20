#ifndef MOUNTER_H
#define MOUNTER_H

// Which filesystem family a recipe describes. Recipes are passed as an array
// indexed by these, so adding a family later changes no signature.
enum
{
	MOUNTFS_FAT,
	MOUNTFS_NTFS,
	MOUNTFS_EXFAT,
	MOUNTFS_CD,
	MOUNTFS_KINDS
};

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

// MountFS flags. These describe what the *handler* can do, so they travel with
// the recipe rather than with the mount session.
#define MOUNTFS_FORCELOAD      0x0001  // prefer 'handler' over a FileSystem.resource entry
                                       // with the same dostype (mountlist ForceLoad=1)
#define MOUNTFS_CD_AUDIO       0x0002  // handler can present audio-only discs (mounted
                                       // non-bootable); without it an audio disc is refused
#define MOUNTFS_CD_ANYFMT      0x0004  // handler identifies disc formats itself (High Sierra,
                                       // UDF, HFS/HFS+ as well as ISO9660); mount a data disc
                                       // that has no ISO9660 PVD instead of rejecting it

// MountStruct flags.
#define MSF_NO_RDB             0x0001  // skip RDB scanning
#define MSF_NO_LEGACY          0x0002  // skip MBR/GPT/superfloppy scanning
#define MSF_NO_CD              0x0004  // skip CD mounting (data discs, audio discs and RDB-CD)
#define MSF_LEGACY_FIRST_ONLY  0x0008  // mount only the first MBR/GPT/superfloppy filesystem per unit
#define MSF_NO_BOOT            0x0010  // never create pre-DOS boot nodes, mount non-bootable
#define MSF_SLOW_SPINUP        0x0020  // allow a slow drive longer to spin up (more read retries)
#define MSF_IGNORE_LAST        0x0040  // keep scanning past a unit whose RDB sets RDBFF_LAST

// What one MountDrive() call did. Optional: pass NULL if none of it is wanted.
struct MountResult
{
	// Volumes mounted across all units.
	LONG mounted;
	// Volumes skipped because their filesystem could not be resolved *yet* — a
	// handler that has to be loaded from L: while there was no dos.library.
	// Zero means nothing is waiting on DOS, so calling MountDrive() again can
	// only re-tread what is already mounted. A pre-DOS boot-ROM caller uses this
	// to decide whether a second pass once DOS exists is worth anything.
	LONG deferred;
	// Volumes found already mounted, on this device, unit and block extent, and
	// therefore left alone. Nonzero is normal on a second pass over a drive that
	// was partly mounted before DOS existed. It is also the only evidence that
	// the duplicate-node guard is doing its job, so it is worth logging.
	LONG alreadyMounted;
	// DOS names that had to be bumped past a collision ("ZZ0" -> "ZZ1").
	// Legitimate when two drives want the same name; the signature of a bug when
	// it is one volume being mounted twice. Worth reporting either way — on a
	// release build the mounter's own logging is compiled out, so this counter is
	// the only way to see it happen.
	LONG renamed;
	// TRUE if at least one unit carried a medium the mounter recognized, even if
	// nothing was mounted from it.
	BOOL recognized;
};

// Everything one MountDrive() call needs. Pure input: the mounter never writes
// to it. Zero-fill it, then set what you need.
struct MountStruct
{
	// Device name. ("myhddriver.device")
	const UBYTE *deviceName;
	// Unit numbers to probe, and how many. Both are required.
	const ULONG *units;
	ULONG unitCount;
	// OPTIONAL: unitCount entries, filled in with each unit's result:
	// -1 = nothing recognized (or the device failed to open), 0 = recognized but
	// nothing mounted, >0 = volumes mounted, -2 = skipped because an earlier
	// unit's RDB set RDBFF_LAST. NULL if the per-unit breakdown is not wanted.
	LONG *unitResults;
	// Name string used to set the Creator field in the FileSystem.resource
	// entries this mount adds. If NULL: use device name.
	const UBYTE *creatorName;
	// ConfigDev: set if autoconfig board autoboot support is wanted.
	// If NULL and bootable partition found: fake ConfigDev is automatically created.
	struct ConfigDev *configDev;
	// SysBase.
	struct ExecBase *SysBase;
	// MSF_* flags.
	ULONG flags;
	// Recommended DMA buffer alignment in bytes (a power of two).
	// Nonzero: recipe-mounted filesystems get their buffers in
	// MEMF_FAST | MEMF_PUBLIC (de_BufMemType) with de_Mask enforcing
	// this alignment.
	// 0: classic behavior (de_BufMemType MEMF_ANY, de_Mask word-aligned).
	ULONG dmaAlign;
	// Filesystem recipes, indexed by MOUNTFS_*. A NULL entry means that family
	// is not mounted, except MOUNTFS_FAT, which falls back to the classic
	// dostype 0x46415401 from FileSystem.resource. MOUNTFS_CD NULL keeps the
	// classic CD behavior (CD01/CDVD from FileSystem.resource, ISO9660 only).
	const struct MountFS *fs[MOUNTFS_KINDS];
};

// Returns the total number of volumes mounted across all units (>0);
// 0 if at least one unit carried a recognized medium but nothing was mounted;
// -1 if no partition table / filesystem was recognized on any unit.
// 'res' may be NULL; see struct MountResult for the per-call breakdown.
LONG MountDrive(const struct MountStruct *ms, struct MountResult *res);

#endif
