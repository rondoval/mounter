
// Generic autoboot/automount RDB parser and mounter.
// - AmigaOS 3.1 (V40) and up.
// - 68000 compatible.
// - Mounts both pre-DOS (from a ROM-resident driver, where the nodes it adds
//   are what strap boots from) and post-DOS (hotplug).
// - Full automount support
// - Full RDB filesystem support.
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

// Copy a C string into a freshly allocated BCPL string: length byte, chars, and
// a trailing NUL (from MEMF_CLEAR) so it doubles as a C string. Used for
// dn_Handler / de_Control, which the DeviceNode owns for its lifetime — so the
// allocation is deliberately never freed.
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

// Buffer for a mount's DOS device name, sized like the RDB pb_DriveName[32]
// field it mirrors: a 32-byte BSTR holds a 31-char name (the RDB limit), and our
// NUL-terminated convention (length byte + chars + NUL) caps usable names at 30.
#define DEVNAME_BUFSIZE 32

// Seed a DOS device-name BSTR (length byte, chars, trailing NUL) into the
// caller's cap-byte buffer.
//
// seedDigit ensures a trailing digit ("UMSD" -> "UMSD0") so units number from 0;
// a name already ending in a digit is left as given. That is what a recipe name
// wants, because every volume the recipe mounts asks for the same one. An RDB
// partition brings its own name from the disk and takes it verbatim — seeding
// there would rename "Work" to "Work0".
//
// fix_name_collision() appends any collision digits later, in the same buffer.
static void make_dos_name(UBYTE *dst, const UBYTE *s, int cap, BOOL seedDigit)
{
	// cap-2 chars fit the buffer (length byte + NUL); when a unit digit may be
	// appended below, reserve one more.
	ULONG len = (ULONG)strlen((const char *)s);
	ULONG maxlen = seedDigit ? (ULONG)(cap - 3) : (ULONG)(cap - 2);
	if (len > maxlen)
		len = maxlen;
	memcpy(dst + 1, s, len);
	if (seedDigit && (len == 0 || dst[len] < '0' || dst[len] > '9')) {
		dst[len + 1] = '0';
		len++;
	}
	dst[0] = (UBYTE)len;
	dst[len + 1] = 0;
}

// Get Block size of unit
static BYTE read_geometry(struct MountData *md, struct IOExtTD *req, struct DriveGeometry *geometry)
{
	struct ExecBase *SysBase = md->SysBase;

	req->iotd_Req.io_Command = TD_GETGEOMETRY;
	req->iotd_Req.io_Data    = geometry;
	req->iotd_Req.io_Length  = sizeof(struct DriveGeometry);

	return DoIO((struct IORequest *)req);
}

// Check block block_checksum
static UWORD block_checksum(UBYTE *buf, struct MountData *md)
{
	ULONG chk = 0;
	ULONG num_longs;

	num_longs = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | (buf[7]);
	// A block's summed-longs count can never exceed the block itself; reject
	// garbage counts so the block_checksum loop can't read past the sector buffer.
	if (num_longs > md->blocksize / sizeof(LONG))
		return FALSE;

	for (UWORD i = 0; i < (int)(num_longs * sizeof(LONG)); i += 4) {
		ULONG v = (buf[i + 0] << 24) | (buf[i + 1] << 16) | (buf[i + 2] << 8) | (buf[i + 3 ] << 0);
		chk += v;
	}
	if (chk) {
		dbg("Checksum error %08lx\n", chk);
		return FALSE;
	}
	return TRUE;
}


#define MAX_RETRIES 3

// Read single block with retries
BOOL mnt_read_block(UBYTE *buf, ULONG block, ULONG id, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct IOExtTD *request = md->request;
	UWORD i, max_retries = MAX_RETRIES;
	if (md->flags & MSF_SLOW_SPINUP)
		max_retries = 15;

	// Byte offsets past 4GB need TD_READ64 (io_Actual = high 32 bits)
	uint64_t offset = (uint64_t)block * (ULONG)md->blocksize;
	request->iotd_Req.io_Command = (offset >> 32) ? TD_READ64 : CMD_READ;
	request->iotd_Req.io_Actual = (ULONG)(offset >> 32);
	request->iotd_Req.io_Offset = (ULONG)offset;
	request->iotd_Req.io_Data = buf;
	request->iotd_Req.io_Length = md->blocksize;
	for (i = 0; i < max_retries; i++) {
		LONG err = DoIO((struct IORequest*)request);
		if (!err) {
			break;
		}
	}
	if (i == max_retries) {
		return FALSE;
	}
	ULONG v = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3] << 0);
	if (id != 0xffffffff) {
		if (v != id) {
			return FALSE;
		}
		if (!block_checksum(buf, md)) {
			return FALSE;
		}
	}
	return TRUE;
}

// Borrow a scratch sector for the duration of one scan step.
//
// The scanners nest — block 0, then a GPT header, then the entry array, then a
// partition's VBR — and each wants a MAX_BLOCKSIZE buffer. They used to AllocMem
// their own, which meant eight allocation sites, eight out-of-memory paths, and
// two allocations *per partition* in the RDB loop. Use is strictly LIFO (every
// buffer is released before its taker returns), so a stack of slots is enough.
//
// Slots are allocated on first use and live until MountDrive() returns, so a plain
// FAT stick never allocates the GPT path's buffers and a 10-partition RDB allocates
// its FSHD sectors once instead of twenty times.
UBYTE *mnt_sector_take(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;

	if (md->sectorsUsed >= SECTOR_SLOTS) {
		printf("Sector pool exhausted\n");
		return NULL;
	}
	UBYTE **slot = &md->sector[md->sectorsUsed];
	if (!*slot) {
		*slot = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);
		if (!*slot)
			return NULL;
	}
	md->sectorsUsed++;
	return *slot;
}

// Release the most recently taken sector. Anything but the current top is a
// programming error: report it and leave the stack alone rather than corrupt it.
void mnt_sector_drop(struct MountData *md, const UBYTE *buf)
{
	if (!md->sectorsUsed || md->sector[md->sectorsUsed - 1] != buf) {
		printf("Sector pool released out of order\n");
		return;
	}
	md->sectorsUsed--;
}

static void sector_pool_free(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;

	for (UWORD i = 0; i < SECTOR_SLOTS; i++) {
		if (md->sector[i]) {
			FreeMem(md->sector[i], MAX_BLOCKSIZE);
			md->sector[i] = NULL;
		}
	}
	md->sectorsUsed = 0;
}

struct FileSysEntry *mnt_find_filesystem(ULONG id1, ULONG id2, struct ExecBase *SysBase)
{
	struct FileSysResource *FileSysResBase = NULL;
	struct FileSysEntry *fse, *fs=NULL;
	Forbid();
	if ((FileSysResBase = (struct FileSysResource *)OpenResource((CONST_STRPTR)FSRNAME))) {
		for (fse = (struct FileSysEntry *)FileSysResBase->fsr_FileSysEntries.lh_Head;
			  fse->fse_Node.ln_Succ;
			  fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
			if ((id1 && fse->fse_DosType==id1) || (id2 && fse->fse_DosType==id2)) {
				fs=fse;
				break;
			}
		}
	}
	Permit();
	return fs;
}

// Give a pre-DOS mount a ConfigDev, without which its BootNodes are not bootable.
//
// AddBootNode() branches on the ConfigDev pointer — expansion.doc: "Autoboot from
// an expansion card before DOS is running requires the card's ConfigDev
// structure.  Pass a NULL ConfigDev pointer to create a non-bootable node."  Only
// a bootable node is NT_BOOTNODE, and only those are candidates for the boot
// scan.  So a caller that mounts before DOS exists — a boot ROM, which is the
// only way to get a BootNode at all — has to supply one even when there is no
// real autoconfig board behind the drive.
//
// Deliberately never freed — the BootNode's LN_NAME points at it for the life of
// the machine.  AllocConfigDev() without AddConfigDev() also keeps it off
// expansion's board list, where romboot scans diag areas for romtags.
static void make_fake_configdev(struct MountData *md)
{
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	extern const struct DiagArea psd_diag_area;   // mounter/bootpoint.c

	md->configDev = AllocConfigDev();
	if (md->configDev) {
		md->configDev->cd_Rom.er_Type |= ERTF_DIAGVALID;
		*(APTR *)&md->configDev->cd_Rom.er_Reserved0c = (APTR)&psd_diag_area;
	}
	dbg("Fake ConfigDev for pre-DOS boot: 0x%08lx (DiagArea 0x%08lx)\n",
	    (ULONG)md->configDev, (ULONG)&psd_diag_area);
}

static UBYTE to_upper(UBYTE c)
{
	if (c >= 'a' && c <= 'z') {
		return c - ('a'-'A');
	}
	return c;
}

// Case-insensitive comparison of a BSTR against len chars. Callers holding a
// second BSTR pass its body and length byte; callers holding a C string pass it
// with strlen(), which makes "same length" and "NUL lands at the end" one test.
static BOOL bstr_equal_ci(const UBYTE *bstr, const UBYTE *chars, UWORD len)
{
	if (*bstr++ != len) {
		return FALSE;
	}
	for (UWORD i = 0; i < len; i++) {
		if (to_upper(bstr[i]) != to_upper(chars[i])) {
			return FALSE;
		}
	}
	return TRUE;
}

// TRUE if this startup packet describes our device, our unit and this exact
// block extent.
static BOOL is_same_extent(struct MountData *md, BPTR startup, ULONG lowCyl, ULONG highCyl)
{
	struct FileSysStartupMsg *fssm = (struct FileSysStartupMsg *)BADDR(startup);
	const UBYTE *dev;
	struct DosEnvec *de;

	if (!startup || !fssm || fssm->fssm_Unit != md->unitnum) {
		return FALSE;
	}
	dev = (const UBYTE *)BADDR(fssm->fssm_Device);
	de  = (struct DosEnvec *)BADDR(fssm->fssm_Environ);
	/* DE_UPPERCYL is the NDK's index name for the de_HighCyl field. */
	if (!dev || !de || de->de_TableSize < DE_UPPERCYL) {
		return FALSE;
	}
	if (de->de_LowCyl != lowCyl || de->de_HighCyl != highCyl) {
		return FALSE;
	}
	return bstr_equal_ci(dev, md->devicename,
	                     (UWORD)strlen((const char *)md->devicename));
}

// TRUE if a DeviceNode for exactly this device, unit and extent already exists.
//
// The pre-DOS pass mounts everything it can reach through FileSystem.resource,
// and massstorage then asks for a re-probe once dos.library exists so that
// partitions needing a *loadable* handler get their second chance. Without this
// test that second pass re-mounts what already worked: fix_name_collision() finds
// the name taken, renames CD0 to CD1, and a second handler opens the same medium
// behind the one DOS is booting from — which surfaces mid-boot as "Please replace
// volume <name> in any drive".
//
// Identity is device + unit + extent rather than the DOS name, so two different
// drives that both want CD0 are still renamed apart exactly as before.
static BOOL is_extent_mounted(struct MountData *md, ULONG lowCyl, ULONG highCyl)
{
	struct ExecBase *SysBase = md->SysBase;
	BOOL found = FALSE;
	struct BootNode *bn;

	Forbid();
	for (bn = (struct BootNode*)md->ExpansionBase->MountList.lh_Head;
		 bn->bn_Node.ln_Succ != NULL;
		 bn = (struct BootNode*)bn->bn_Node.ln_Succ)
	{
		struct DeviceNode *dn = bn->bn_DeviceNode;
		if (dn && is_same_extent(md, dn->dn_Startup, lowCyl, highCyl)) {
			found = TRUE;
			break;
		}
	}
	Permit();

	// Post-boot mounts go straight to the DOS lists, not eb_MountList.
	if (!found && md->DOSBase) {
		struct DosLibrary *DOSBase = md->DOSBase;
		struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
		while ((dl = NextDosEntry(dl, LDF_DEVICES))) {
			if (is_same_extent(md, (BPTR)dl->dol_misc.dol_handler.dol_Startup, lowCyl, highCyl)) {
				found = TRUE;
				break;
			}
		}
		UnLockDosList(LDF_DEVICES | LDF_READ);
	}

	if (found) {
		dbg("Extent %lu..%lu on unit %lu already mounted\n", lowCyl, highCyl, md->unitnum);
	}
	return found;
}

// Check for duplicate device names
static BOOL is_name_taken(struct MountData *md, UBYTE *bname)
{
	struct ExecBase *SysBase = md->SysBase;
	BOOL found = FALSE;

	Forbid();
	struct BootNode *bn;
	for (bn = (struct BootNode*)md->ExpansionBase->MountList.lh_Head;
		 bn->bn_Node.ln_Succ != NULL;
		 bn = (struct BootNode*)bn->bn_Node.ln_Succ)
	{
		struct DeviceNode *dn = bn->bn_DeviceNode;
		const UBYTE *bname2 = BADDR(dn->dn_Name);
		if (bstr_equal_ci(bname, bname2 + 1, bname2[0])) {
			found = TRUE;
		}
	}

	Permit();

	// Post-boot mounts go straight to the DOS lists, not eb_MountList — check
	// those too (devices, volumes and assigns all claim the name). No DOSBase
	// means we are running before DOS, where eb_MountList is the whole picture.
	if (!found && md->DOSBase) {
		struct DosLibrary *DOSBase = md->DOSBase;
		struct DosList *dl = LockDosList(LDF_ALL | LDF_READ);
		if (FindDosEntry(dl, (STRPTR)(bname + 1), LDF_ALL)) {
			found = TRUE;
		}
		UnLockDosList(LDF_ALL | LDF_READ);
	}
	return found;
}

// Bump the trailing decimal number ("UMSD0" -> "UMSD1", "UMSD9" -> "UMSD10")
// until the name collides with nothing. A name without a trailing number
// (RDB partition names) gets "1" appended on its first collision.
//
// bname is a NUL-terminated BSTR (length byte, chars, trailing NUL) living in
// a buffer of 'cap' bytes, so the longest name it can hold is cap-2 chars (one
// byte for the length prefix, one for the NUL). Every write below is bounded by
// that limit: the tail digit lands at most at name[maxlen-1], and a grown name
// plus its NUL occupy through name[maxlen] == bname[cap-1], the last byte.
//
// A collision is legitimate when another drive wants the same name, and is the
// signature of a bug when it is the same volume being mounted twice (ZZ0 gaining
// a live ZZ1). The two are told apart before we get here, by
// is_extent_mounted(); every bump that still happens is counted so the caller
// can report it, because on a release build none of the logging below exists and
// this failure is otherwise completely silent.
static void fix_name_collision(struct MountData *md, UBYTE *bname, int cap)
{
	int maxlen = cap - 2;
	while (is_name_taken(md, bname)) {
		UBYTE len = bname[0] > maxlen ? (UBYTE)maxlen : bname[0];
		UBYTE *name = bname + 1;
		md->renamed++;
		dbg("Duplicate device name '%s'\n", name);
		WORD pos = len - 1;
		while (pos >= 0 && name[pos] == '9') {
			name[pos--] = '0';
		}
		if (pos >= 0 && name[pos] >= '0' && name[pos] <= '8') {
			name[pos]++;
		} else if (len < maxlen) {
			// no digit to bump: insert '1' before the carried zeros
			// (appends when there was no trailing number at all)
			for (WORD i = len; i > pos + 1; i--) {
				name[i] = name[i - 1];
			}
			name[pos + 1] = '1';
			name[++len] = 0;
			bname[0] = len;
		} else {
			// no room to grow; give up rather than loop forever
			break;
		}
		dbg("-> new device name '%s'\n", name);
	}
}

// Add a DeviceNode to the system.
//
// A BootNode is only NT_BOOTNODE — and so only reachable by strap — when a
// ConfigDev is supplied, which is what makes this the pre-DOS boot path (see
// make_fake_configdev). Post-DOS, or for anything not meant to be booted, a
// NULL ConfigDev is exactly what the pre-V36 AddDosNode() meant: expansion.doc
// calls it "the old (pre V36) function that works just like AddBootNode()".
// So one call covers both worlds.
static void add_mount_node(struct MountData *md, LONG bootPri, struct DeviceNode *dn)
{
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	struct ConfigDev *cd = NULL;

	if (!md->DOSBase && bootPri > MOUNT_NEVER_BOOT && !(md->flags & MSF_NO_BOOT))
		cd = md->configDev;
	dbg("Mounting %s: pri %ld\n", cd ? "bootable" : "non-bootable", bootPri);
	AddBootNode(bootPri, ADNF_STARTPROC, dn, cd);
}

static void apply_patch_flags(struct DeviceNode *dn, struct FileSysEntry *fse)
{
	// Process PatchFlags.
	ULONG patchFlags = fse->fse_PatchFlags;
	if (patchFlags & 0x0001)
		dn->dn_Type = fse->fse_Type;
	if (patchFlags & 0x0002)
		dn->dn_Task = (struct MsgPort *)fse->fse_Task;
	if (patchFlags & 0x0004)
		dn->dn_Lock = fse->fse_Lock;
	if (patchFlags & 0x0008)
		dn->dn_Handler = fse->fse_Handler;
	if (patchFlags & 0x0010)
		dn->dn_StackSize = fse->fse_StackSize;
	if (patchFlags & 0x0020)
		dn->dn_Priority = fse->fse_Priority;
	if (patchFlags & 0x0040)
		dn->dn_Startup = fse->fse_Startup;
	if (patchFlags & 0x0080)
		dn->dn_SegList = fse->fse_SegList;
	if (patchFlags & 0x0100)
		dn->dn_GlobalVec = fse->fse_GlobalVec;
}

// TRUE if the recipe can resolve to a working filesystem *now*: the dostype is
// registered in FileSystem.resource, or a handler file is given and can be
// found on disk. Checked before creating a DeviceNode so unresolvable
// partitions are skipped clean, rather than leaving a node that fails the
// moment something touches it (a handler the user never installed).
//
// "Now" is the load-bearing word before DOS exists. A handler path is a promise
// only DOS can keep, so pre-DOS the answer is FALSE even for a recipe that names
// one: the volume is not unmountable, it is not mountable *yet*. Saying TRUE here
// used to leave a dead DeviceNode holding the DOS name, so the post-DOS pass that
// could have mounted the volume properly found the name taken, renamed it, and
// mounted a second node beside the dead one (ZZ0 gaining a ZZ1). The caller
// counts a FALSE from here as deferred and comes back once DOS is up.
//
// The file check itself needs DOS and a Process (Lock() is a packet), so a
// task-context caller that does have DOS still takes the handler on trust.
static BOOL is_fs_available(struct MountData *md, const struct MountFS *fs)
{
	struct ExecBase *SysBase = md->SysBase;

	if (mnt_find_filesystem(fs->dosType, 0, SysBase) != NULL)
		return TRUE;
	if (!fs->handler)
		return FALSE;
	if (!md->DOSBase)
		return FALSE;
	if (SysBase->ThisTask->tc_Node.ln_Type == NT_PROCESS) {
		struct DosLibrary *DOSBase = md->DOSBase;
		struct Process *me = (struct Process *)SysBase->ThisTask;
		// A handler path behind a missing assign must not pop a requester
		// at the user in the middle of a hotplug mount.
		APTR oldwin = me->pr_WindowPtr;
		me->pr_WindowPtr = (APTR)-1;
		BPTR lock = Lock((STRPTR)fs->handler, ACCESS_READ);
		me->pr_WindowPtr = oldwin;
		if (!lock)
			return FALSE;
		UnLock(lock);
	}
	return TRUE;
}

// Attach handler information to a fresh DeviceNode: the FileSystem.resource
// entry if the dostype is registered, else the recipe's handler file (loaded
// by DOS on first access). MOUNTFS_FORCELOAD swaps that order for recipes that
// must not lose to a stale controller-ROM filesystem sitting on their dostype
// (what ForceLoad=1 does in a mountlist), keeping the resource entry as the
// fallback for machines that carry the handler in ROM and nowhere else.
//
// Not pre-DOS, though. There a handler path is a promise nobody can keep: the
// node would carry "L:ODFileSystem" with no DOS to load it, so strap cannot boot
// the volume however good the handler would have been. The resource entry is the
// only thing that can work before DOS exists, so it wins there — which is the
// whole point of putting a filesystem in the Kickstart in the first place.
//
// And if there is no resource entry, pre-DOS this must fail rather than fall back
// to the handler path: a node that cannot be serviced still claims the DOS name,
// which is exactly what turned the later, working mount into a renamed duplicate.
// is_fs_available() already refuses such a recipe before we get here; this
// stays consistent with it so the two cannot drift apart.
static BOOL attach_fs(struct MountData *md, struct DeviceNode *dn, const struct MountFS *fs)
{
	struct ExecBase *SysBase = md->SysBase;
	struct FileSysEntry *fse = mnt_find_filesystem(fs->dosType, 0, SysBase);
	BOOL forceLoad = md->DOSBase && fs->handler && (fs->fsFlags & MOUNTFS_FORCELOAD);

	if (fse && !forceLoad) {
		apply_patch_flags(dn, fse);
		return TRUE;
	}
	if (fs->handler && md->DOSBase) {
		UBYTE *hb = bstr_alloc(fs->handler, SysBase);
		if (hb) {
			dn->dn_Handler = MKBADDR(hb);
			dn->dn_GlobalVec = (BPTR)-1;   /* C handler convention */
			dn->dn_StackSize = fs->stackSize ? fs->stackSize : 8192;
			return TRUE;
		}
	}
	if (fse) {
		apply_patch_flags(dn, fse);
		return TRUE;
	}
	return FALSE;
}

// Fill a DosEnvec for a recipe-mounted volume from a block extent. lowCyl/highCyl
// bound the volume in blocks (Surfaces/SectorPerBlock/BlocksPerTrack are all 1, so
// one block is one "cylinder"); pass 0/0 for a whole-medium device such as a CD.
// The RDB path does not use this — its DosEnvec comes off the disk.
// Expects a zeroed DosEnvec: the fields not set here stay at 0.
void mnt_envec_from_recipe(struct MountData *md, struct DosEnvec *de,
                              const struct MountFS *fs, ULONG lowCyl, ULONG highCyl,
                              LONG bootPri)
{
	de->de_TableSize      = 16; // up to DE_DOSTYPE
	de->de_SizeBlock      = md->blocksize >> 2;
	de->de_Surfaces       = 1;
	de->de_SectorPerBlock = 1;
	de->de_BlocksPerTrack = 1;
	de->de_LowCyl         = lowCyl;
	de->de_HighCyl        = highCyl;
	de->de_NumBuffers     = fs->buffers ? fs->buffers : 5;
	de->de_MaxTransfer    = fs->maxTransfer ? fs->maxTransfer : 0x100000;
	/* When the host controller reports a DMA alignment, put the filesystem's
	 * buffer cache in Fast memory and require that alignment
	 * via de_Mask. Otherwise keep classic behavior. */
	if (md->dmaAlign > 1) {
		de->de_BufMemType = MEMF_FAST|MEMF_PUBLIC|MEMF_CLEAR;
		de->de_Mask       = 0x7FFFFFFE & ~((ULONG)md->dmaAlign - 1);
	} else {
		de->de_BufMemType = MEMF_ANY|MEMF_CLEAR;
		de->de_Mask       = 0x7FFFFFFE;
	}
	de->de_DosType        = fs->dosType;
	de->de_BootPri        = bootPri;
	if (fs->control) {
		UBYTE *cb = bstr_alloc(fs->control, md->SysBase);   /* lives in the DeviceNode */
		if (cb) {
			de->de_Control   = (ULONG)MKBADDR(cb);
			de->de_TableSize = 19;   // up to de_BootBlocks, includes de_Control
		}
	}
}

// TRUE if this recipe can be mounted right now.
//
// A recipe that names a handler file is not unmountable when there is no DOS to
// load it — it is unmountable *yet*, so it is counted as deferred and the caller
// comes back once DOS is up. Mounting a placeholder node instead would claim the
// DOS name and force that later, working mount into a renamed duplicate.
BOOL mnt_resolve_fs(struct MountData *md, const struct MountFS *fs, ULONG lowCyl)
{
	(void)lowCyl; // In case we turned debugging off.

	if (is_fs_available(md, fs))
		return TRUE;

	printf("No filesystem for dostype 0x%08lx (%s) at block %lu\n", fs->dosType,
	       fs->handler ? (const char *)fs->handler : (const char *)"no handler", lowCyl);
	if (!md->DOSBase && fs->handler)
		md->deferred++;
	return FALSE;
}

// Create and add the DeviceNode for one volume. Every scanner ends here, so the
// duplicate guard, the naming rules, the filesystem attach and the boot-node
// decision each exist exactly once.
//
// The extent check must stay first: a volume already mounted on an earlier pass
// must never reach the naming step. The other order bumped the DOS name — and
// md->renamed with it — for a volume the mounter then declined to mount, which
// is exactly the signal md->renamed exists to give.
//
// Increments md->mounted for anything that leaves a live node behind, so no caller
// has to remember to count.
enum MountOutcome mnt_mount_volume(struct MountData *md, struct Volume *vol)
{
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;   /* MakeDosNode base */
	UBYTE name[DEVNAME_BUFSIZE];

	if (is_extent_mounted(md, vol->pp.de.de_LowCyl, vol->pp.de.de_HighCyl)) {
		md->alreadyMounted++;
		md->mounted++;
		return MOUNT_ALREADY;
	}

	// A recipe name is shared by every volume that recipe mounts, so it carries a
	// unit digit; an RDB partition brings a name of its own and keeps it verbatim.
	make_dos_name(name, vol->nameHint, sizeof(name), vol->fs != NULL);
	fix_name_collision(md, name, sizeof(name));

	vol->pp.dosname  = name + 1;
	vol->pp.execname = md->devicename;
	vol->pp.unitnum  = md->unitnum;

	struct DeviceNode *dn = MakeDosNode(&vol->pp);
	if (!dn) {
		printf("Could not create DosNode\n");
		return MOUNT_FAILED;
	}
	if (vol->fs) {
		// Recipe: the handler has to be resolvable or the node is dead weight.
		if (!attach_fs(md, dn, vol->fs)) {
			printf("Could not load filesystem\n");
			return MOUNT_FAILED;
		}
	} else if (vol->fse) {
		// RDB: the filesystem came off the disk, or from FileSystem.resource.
		apply_patch_flags(dn, vol->fse);
	}
	add_mount_node(md, vol->bootPri, dn);
	md->mounted++;
	return MOUNT_OK;
}

// Probe one unit: open it, read geometry, scan its partition table(s), close.
// The scanners answer one question each — did you recognize this medium? — and
// leave the count in md->mounted, so the -1/0/count encoding is applied here, once.
// Returns -1 if nothing recognized the medium, else the number mounted.
static LONG probe_unit(struct MountData *md, const struct MountStruct *ms, ULONG unitNum,
                      struct IOExtTD *request)
{
	struct ExecBase *SysBase = md->SysBase;
	struct DriveGeometry geom;
	BOOL recognized = FALSE;
	BYTE err;

	dbg("OpenDevice('%s', %ld, 0x%08lx, 0)\n", ms->deviceName, unitNum, (ULONG)request);
	err = OpenDevice(ms->deviceName, unitNum, (struct IORequest*)request, 0);
	if (err != 0) {
		dbg("OpenDevice(%s,%ld) failed: %ld\n", ms->deviceName, unitNum, (LONG)err);
		return -1;
	}
	if (read_geometry(md, request, &geom) == 0) {
		if (geom.dg_SectorSize < 256 || geom.dg_SectorSize > MAX_BLOCKSIZE) {
			printf("Unsupported sector size %lu.\n", geom.dg_SectorSize);
			goto out;
		}
		md->request      = request;
		md->devicename   = ms->deviceName;
		md->blocksize    = geom.dg_SectorSize;
		md->totalsectors = geom.dg_TotalSectors;
		md->unitnum      = unitNum;
		md->legacyMounted = FALSE;
		md->mounted      = 0;
		switch (geom.dg_DeviceType & SID_TYPE) {
		case DG_CDROM:
		case DG_WORM:
		case DG_OPTICAL_DISK:
			if (!(md->flags & MSF_NO_CD))
				recognized = mnt_scan_cd(md);
			else {
				printf("CDROM mounting disabled.\n");
			}
			break;
		case DG_DIRECT_ACCESS:
			if (!(md->flags & MSF_NO_RDB))
				recognized = mnt_scan_rdb(md);
			if (!recognized && !(md->flags & MSF_NO_LEGACY))
				recognized = mnt_scan_legacy(md);
			break;
		default:
			printf("Don't know how to boot from device type %ld.\n", (LONG)(geom.dg_DeviceType & SID_TYPE));
			break;
		}
	}
out:
	// Every scanner releases what it takes; a leak would shrink the pool for the
	// units still to come.
	if (md->sectorsUsed) {
		printf("Sector pool leak: %ld slot(s)\n", (LONG)md->sectorsUsed);
		md->sectorsUsed = 0;
	}
	// Disable motor after probing (md->request is unset if geometry failed)
	request->iotd_Req.io_Command = TD_MOTOR;
	request->iotd_Req.io_Length  = 0;
	DoIO((struct IORequest*)request);
	CloseDevice((struct IORequest*)request);
	return recognized ? md->mounted : -1;
}

// Explicit unit(s): a single unit number (< 0x100), else a pointer to a
// { count, unit0, unit1, ... } array. Lets a caller mount known units (e.g. a
// hotplug driver) instead of scanning. For an array, each entry is overwritten
// with that unit's result (-2 = skipped after a prior RDBFF_LAST).
static void scan_units(struct MountData *md, const struct MountStruct *ms,
                       struct IOExtTD *request, LONG *total, BOOL *recognized)
{
	BOOL skipRest = FALSE;

	for (ULONG i = 0; i < ms->unitCount; i++) {
		LONG r = -2;                    /* skipped after an earlier RDBFF_LAST */
		if (!skipRest) {
			r = probe_unit(md, ms, ms->units[i], request);
			if (r >= 0) {
				*recognized = TRUE;
				*total += r;
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

// The one way in; see mounter.h for the contract.
// 0 = recognized but nothing mounted, >0 = partitions mounted,
// -2 = skipped because a previous unit had RDBFF_LAST set.
LONG MountDrive(const struct MountStruct *ms, struct MountResult *res)
{
	struct ExecBase *SysBase = ms->SysBase;
	struct ExpansionBase *ExpansionBase = NULL;
	struct MountData *md = NULL;
	struct MsgPort *port = NULL;
	struct IOExtTD *request = NULL;
	LONG total = 0;
	BOOL recognized = FALSE;

	dbg("Starting..\n");

	if (res)
		memset(res, 0, sizeof(*res));

	if (!ms->units || !ms->unitCount) {
		printf("MountDrive: no units given\n");
		return -1;
	}

	ExpansionBase = (struct ExpansionBase*)OpenLibrary((CONST_STRPTR)"expansion.library", 40);
	if (!ExpansionBase)
		goto cleanup;

	md = AllocMem(sizeof(struct MountData), MEMF_CLEAR | MEMF_PUBLIC);
	if (!md)
		goto cleanup;

	md->DOSBase = (struct DosLibrary*)OpenLibrary((CONST_STRPTR)"dos.library", 40);
	md->SysBase = SysBase;
	md->ExpansionBase = ExpansionBase;
	dbg("SysBase=0x%08lx ExpansionBase=0x%08lx DosBase=0x%08lx\n", (ULONG)md->SysBase, (ULONG)md->ExpansionBase, (ULONG)md->DOSBase);
	md->configDev = ms->configDev;
	md->creator = ms->creatorName;
	md->flags = ms->flags;

	// Before DOS exists every node we add goes on the boot list, and that needs a
	// ConfigDev to be an NT_BOOTNODE at all (see make_fake_configdev).  Done here,
	// once per mount, so every add_mount_node() is covered — callers that have a
	// real board still pass their own in ms->configDev.
	if (!md->configDev && !md->DOSBase && !(md->flags & MSF_NO_BOOT))
		make_fake_configdev(md);

	for (int i = 0; i < MOUNTFS_KINDS; i++)
		md->fs[i] = ms->fs[i];
	md->dmaAlign = ms->dmaAlign;

	port = CreateMsgPort();
	if (!port)
		goto cleanup;

	request = (struct IOExtTD*)CreateIORequest(port, sizeof(struct IOExtTD));
	if (!request)
		goto cleanup;

	scan_units(md, ms, request, &total, &recognized);

cleanup:
	if (request)
		DeleteIORequest(request);
	if (port)
		DeleteMsgPort(port);
	if (md) {
		sector_pool_free(md);
		if (res) {
			res->mounted        = total;
			res->deferred       = md->deferred;
			res->alreadyMounted = md->alreadyMounted;
			res->renamed        = md->renamed;
			res->recognized     = recognized;
		}
		if (md->DOSBase)
			CloseLibrary(&md->DOSBase->dl_lib);
		FreeMem(md, sizeof(struct MountData));
	}
	if (ExpansionBase)
		CloseLibrary(&ExpansionBase->LibNode);

	LONG ret = total > 0 ? total : (recognized ? 0 : -1);
	dbg("Exit code %ld\n", ret);
	return ret;
}
