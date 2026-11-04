
// mounter_fs.c — filesystems: FileSystem.resource lookups and the entries an
// RDB's FSHD blocks add to it, the choice between a resource entry and a
// recipe's handler file, and attaching that choice to a DeviceNode.
//
// Part of the generic autoboot/automount mounter; see mounter.c for the
// copyright and licence covering this file.
#include "mounter_internal.h"

// PatchFlags bit n stands for the nth long from dn_Type: dn_Type, dn_Task,
// dn_Lock, dn_Handler, dn_StackSize, dn_Priority, dn_Startup, dn_SegList and
// dn_GlobalVec. A FileSysEntry (from fse_Type) and an FSHD block (from fhb_Type)
// hold the same nine longs in the same order.
#define PATCH_LONGS 9

_Static_assert(offsetof(struct DeviceNode, dn_GlobalVec) -
               offsetof(struct DeviceNode, dn_Type) == (PATCH_LONGS - 1) * sizeof(ULONG),
               "DeviceNode patch longs");
_Static_assert(offsetof(struct FileSysEntry, fse_GlobalVec) -
               offsetof(struct FileSysEntry, fse_Type) == (PATCH_LONGS - 1) * sizeof(ULONG),
               "FileSysEntry patch longs");
_Static_assert(offsetof(struct FileSysHeaderBlock, fhb_GlobalVec) -
               offsetof(struct FileSysHeaderBlock, fhb_Type) == (PATCH_LONGS - 1) * sizeof(ULONG),
               "FileSysHeaderBlock patch longs");

/**
 * patch_longs() - Copy the longs a PatchFlags word selects.
 * @dst:        the first of the nine patch longs to write (dn_Type or fse_Type)
 * @src:        the first of the nine to read (fse_Type or fhb_Type)
 * @patchFlags: bit n copies long n; bits 9 and up are ignored
 *
 * Copied with memcpy(): the nine fields are of several types (pointers, BPTRs,
 * BSTRs, numbers), all one long wide.
 */
static void patch_longs(void *dst, const void *src, ULONG patchFlags)
{
	for (UWORD i = 0; i < PATCH_LONGS; i++) {
		if (patchFlags & (1UL << i))
			memcpy((UBYTE *)dst + i * sizeof(ULONG), (const UBYTE *)src + i * sizeof(ULONG),
			       sizeof(ULONG));
	}
}

/**
 * bstr_alloc() - Copy a C string into a freshly allocated BCPL string.
 * @s:       NUL-terminated C string; only its first 255 chars are kept, the most
 *           a BSTR's single length byte can count
 * @SysBase: exec
 *
 * The result is the length byte, the chars and a trailing NUL (from MEMF_CLEAR),
 * so it doubles as a C string. Used for dn_Handler and de_Control, which the
 * DeviceNode owns for its lifetime once added — so only a BSTR no node took is
 * freed (mnt_release_fs()).
 *
 * Return: the BSTR as a byte pointer (AllocMem() memory, so MKBADDR() of it is
 * valid); NULL if AllocMem() fails
 */
static UBYTE *bstr_alloc(const UBYTE *s, struct ExecBase *SysBase)
{
	ULONG len = (ULONG)strlen((const char *)s);
	if (len > 255)      // a BSTR length prefix is a single byte
		len = 255;
	UBYTE *b = AllocMem(len + 2, MEMF_PUBLIC | MEMF_CLEAR);
	if (b) {
		b[0] = (UBYTE)len;
		memcpy(b + 1, s, len);
	}
	return b;
}

/**
 * bstr_free() - Free a BSTR from bstr_alloc().
 * @b:       the BSTR as a byte pointer; NULL accepted
 * @SysBase: exec
 */
static void bstr_free(UBYTE *b, struct ExecBase *SysBase)
{
	if (b)
		FreeMem(b, (ULONG)b[0] + 2);
}

/**
 * mnt_find_filesystem() - Look a dostype up in FileSystem.resource.
 * @id1:     dostype to find; 0 matches nothing
 * @id2:     a second dostype accepted on equal terms (CDVD beside CD01); 0 for
 *           none
 * @SysBase: exec
 *
 * Walks fsr_FileSysEntries under Forbid(). The first entry in list order that
 * matches either id wins, so @id1 has no precedence over @id2.
 *
 * The entry stays valid after the Forbid() ends: nothing removes entries from
 * FileSystem.resource, since DOS devices run the code they point at. The mount
 * path relies on that, reading the entry long after the lookup.
 *
 * FileSystem.resource is a Kickstart resident from V40 on, so it is taken as
 * given: if it is somehow absent, nothing is found, and the mounter does not
 * make one, which would shadow the real one for everything mounted later.
 *
 * Return: the matching FileSysEntry; NULL if none matches or there is no
 * FileSystem.resource
 */
struct FileSysEntry *mnt_find_filesystem(ULONG id1, ULONG id2, struct ExecBase *SysBase)
{
	struct FileSysResource *FileSysResBase;
	struct FileSysEntry *found = NULL;

	Forbid();
	if ((FileSysResBase = (struct FileSysResource *)OpenResource((CONST_STRPTR)FSRNAME))) {
		for (struct FileSysEntry *fse = (struct FileSysEntry *)FileSysResBase->fsr_FileSysEntries.lh_Head;
		     fse->fse_Node.ln_Succ;
		     fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
			if ((id1 && fse->fse_DosType == id1) || (id2 && fse->fse_DosType == id2)) {
				found = fse;
				break;
			}
		}
	}
	Permit();
	return found;
}

/**
 * mnt_fse_new() - Build a FileSysEntry from an RDB FileSysHeaderBlock.
 * @md:   mount state; md->creator is copied behind the entry as its ln_Name
 * @fshb: the FSHD block as read off the disk; only read
 *
 * Nothing is built when FileSystem.resource already carries an entry for
 * fhb_DosType at fhb_Version or newer. The fields fhb_PatchFlags selects are
 * copied as they are; with bit 0x0080 that puts the fhb_SegListBlocks block
 * number in fse_SegList, which the caller overwrites. No Forbid() is needed:
 * the lookup takes its own, and the new entry is the caller's alone until
 * mnt_fse_register() adds it, which checks the resource again.
 *
 * Return: a fresh, unregistered entry with no seglist yet — the caller loads the
 * filesystem into it and then hands it to mnt_fse_register(); NULL if the
 * resource's existing entry is good enough, if the allocation fails, or if
 * FileSystem.resource cannot be reached at all
 */
struct FileSysEntry *mnt_fse_new(struct MountData *md, const struct FileSysHeaderBlock *fshb)
{
	struct ExecBase *SysBase = md->SysBase;

	if (!OpenResource((CONST_STRPTR)FSRNAME))
		return NULL;
	struct FileSysEntry *existing = mnt_find_filesystem(fshb->fhb_DosType, 0, SysBase);
	if (existing && existing->fse_Version >= fshb->fhb_Version) {
		dbg("FileSystem.resource: entry 0x%08lx for %08lx is version 0x%08lx >= 0x%08lx, keeping it\n",
		    (ULONG)existing, fshb->fhb_DosType, existing->fse_Version, fshb->fhb_Version);
		return NULL;
	}
	struct FileSysEntry *fse = AllocVec(sizeof(struct FileSysEntry) + strlen((const char *)md->creator) + 1,
	                                    MEMF_PUBLIC | MEMF_CLEAR);
	if (!fse)
		return NULL;
	patch_longs(&fse->fse_Type, &fshb->fhb_Type, fshb->fhb_PatchFlags);
	fse->fse_DosType = fshb->fhb_DosType;
	fse->fse_Version = fshb->fhb_Version;
	fse->fse_PatchFlags = fshb->fhb_PatchFlags;
	strcpy((char *)(fse + 1), (const char *)md->creator);
	fse->fse_Node.ln_Name = (char *)(fse + 1);
	dbg("FileSystem.resource: new FileSysEntry 0x%08lx for %08lx from FSHD\n",
	    (ULONG)fse, fshb->fhb_DosType);
	return fse;
}

/**
 * mnt_fse_register() - Add a new FileSysEntry to FileSystem.resource, or free it.
 * @md:  mount state
 * @fse: an entry from mnt_fse_new() with fse_SegList set, 0 if the filesystem
 *       load failed; the resource owns it once added, else it is freed here
 *
 * The check against the resource and the AddHead() are one Forbid(), so two
 * calls loading the same filesystem off two disks at once add it once: the one
 * that comes second finds the first's entry, at the same version, and gives
 * way. AddHead() puts the entry ahead of any older one for the dostype, so
 * mnt_find_filesystem() finds it first.
 *
 * Return: TRUE if the entry was added (and is still valid to read); FALSE if it
 * was freed — the load failed, there is no resource, or an entry for the dostype
 * at this version or newer is there by now. On FALSE the caller must drop its
 * pointer, and free the seglist it loaded
 */
BOOL mnt_fse_register(struct MountData *md, struct FileSysEntry *fse)
{
	struct ExecBase *SysBase = md->SysBase;

	if (fse->fse_SegList) {
		Forbid();
		struct FileSysResource *fsr = OpenResource((CONST_STRPTR)FSRNAME);
		struct FileSysEntry *existing = fsr ? mnt_find_filesystem(fse->fse_DosType, 0, SysBase) : NULL;
		if (fsr && !(existing && existing->fse_Version >= fse->fse_Version)) {
			AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
			Permit();
			dbg("FileSysEntry 0x%08lx added to FileSystem.resource, dostype %08lx\n", (ULONG)fse, fse->fse_DosType);
			return TRUE;
		}
		Permit();
	}
	dbg("FileSysEntry 0x%08lx freed, dostype %08lx\n", (ULONG)fse, fse->fse_DosType);
	FreeVec(fse);
	return FALSE;
}

/**
 * handler_found() - Tell whether a recipe's handler file can be loaded now.
 * @md:      mount state; a NULL md->DOSBase means pre-DOS
 * @handler: the recipe's handler path; NULL accepted
 *
 * "Now" is the load-bearing word before DOS exists. A handler path is a promise
 * only DOS can keep, so pre-DOS the answer is FALSE: the volume is not
 * unmountable, it is not mountable *yet*. A node carrying such a path would claim
 * the DOS name, and the post-DOS pass that could have mounted the volume would
 * find the name taken and mount a renamed second node beside the dead one.
 *
 * The file check itself needs DOS and a Process (Lock() is a packet), so a
 * task-context caller that does have DOS takes the handler on trust.
 *
 * Return: TRUE if DOS is open, @handler is given and Lock() finds it (or the
 * caller is a Task); FALSE otherwise, with requesters suppressed
 */
static BOOL handler_found(struct MountData *md, const UBYTE *handler)
{
	struct ExecBase *SysBase = md->SysBase;

	if (!handler || !md->DOSBase)
		return FALSE;
	if (SysBase->ThisTask->tc_Node.ln_Type != NT_PROCESS)
		return TRUE;

	struct DosLibrary *DOSBase = md->DOSBase;
	struct Process *me = (struct Process *)SysBase->ThisTask;
	// A handler path behind a missing assign must not pop a requester at the
	// user in the middle of a hotplug mount.
	APTR oldwin = me->pr_WindowPtr;
	me->pr_WindowPtr = (APTR)-1;
	BPTR lock = Lock((STRPTR)handler, ACCESS_READ);
	me->pr_WindowPtr = oldwin;
	if (!lock)
		return FALSE;
	UnLock(lock);
	return TRUE;
}

/**
 * mnt_resolve_fs() - Decide what a recipe volume's DeviceNode will run.
 * @md: mount state; a NULL md->DOSBase means pre-DOS
 * @fs: the volume's recipe
 * @fc: out: the choice, and the BSTRs it allocated; mnt_attach_fs() hands them
 *      to a node, mnt_release_fs() frees them if no node takes them
 *
 * The FileSystem.resource entry if the dostype is registered, else the recipe's
 * handler file if handler_found() can find it, for DOS to load on first access.
 * MOUNTFS_FORCELOAD swaps that order, for recipes that must not lose to a stale
 * controller-ROM filesystem sitting on their dostype (what ForceLoad=1 does in a
 * mountlist); the resource entry stays the fallback for machines that carry the
 * handler in ROM and nowhere else. Pre-DOS only the resource entry can work: it
 * is the whole point of putting a filesystem in the Kickstart.
 *
 * A recipe that resolves to neither is skipped before any node exists, rather
 * than leaving a node that fails the moment something touches it. The recipe's
 * de_Control string is allocated as well; when that allocation fails the volume
 * only goes without it.
 *
 * Return: MOUNT_OK with a resource entry or a handler chosen; MOUNT_DEFERRED
 * before DOS exists, when only the recipe's handler file could serve;
 * MOUNT_FAILED when nothing can (no entry, and no handler or none that can be
 * found or copied)
 */
enum MountOutcome mnt_resolve_fs(struct MountData *md, const struct MountFS *fs,
                                 struct FsChoice *fc)
{
	struct ExecBase *SysBase = md->SysBase;

	memset(fc, 0, sizeof(*fc));
	fc->fse = mnt_find_filesystem(fs->dosType, 0, SysBase);
	if (!fc->fse || (fs->fsFlags & MOUNTFS_FORCELOAD)) {
		if (handler_found(md, fs->handler)) {
			fc->handler = bstr_alloc(fs->handler, SysBase);
			fc->stackSize = fs->stackSize ? fs->stackSize : 8192;
		}
		if (!fc->handler && !fc->fse)
			return (!md->DOSBase && fs->handler) ? MOUNT_DEFERRED : MOUNT_FAILED;
	}
	if (fs->control)
		fc->control = bstr_alloc(fs->control, SysBase);
	return MOUNT_OK;
}

/**
 * mnt_attach_fs() - Attach a filesystem choice to a fresh DeviceNode.
 * @dn: the node from MakeDosNode(), not yet added
 * @fc: the choice: the handler file wins when set (dn_Handler, dn_GlobalVec -1,
 *      dn_StackSize), else a resource entry goes in through its PatchFlags; with
 *      neither the node keeps MakeDosNode()'s ROM filesystem
 *
 * The node takes over fc->handler; fc->control has already gone into the
 * DosEnvec MakeDosNode() copied.
 */
void mnt_attach_fs(struct DeviceNode *dn, const struct FsChoice *fc)
{
	if (fc->handler) {
		dn->dn_Handler = MKBADDR(fc->handler);
		dn->dn_GlobalVec = (BPTR)-1;   /* C handler convention */
		dn->dn_StackSize = fc->stackSize;
	} else if (fc->fse) {
		patch_longs(&dn->dn_Type, &fc->fse->fse_Type, fc->fse->fse_PatchFlags);
	}
}

/**
 * mnt_release_fs() - Free the BSTRs of a filesystem choice no node took.
 * @md: mount state
 * @fc: the choice from mnt_resolve_fs(); its handler and control are freed and
 *      cleared
 */
void mnt_release_fs(struct MountData *md, struct FsChoice *fc)
{
	struct ExecBase *SysBase = md->SysBase;

	bstr_free(fc->handler, SysBase);
	bstr_free(fc->control, SysBase);
	fc->handler = NULL;
	fc->control = NULL;
}
