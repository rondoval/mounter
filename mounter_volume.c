// SPDX-License-Identifier: BSD-2-Clause
// mounter_volume.c — volumes into DOS devices: the one path that creates a
// DeviceNode, with the duplicate guard, the DOS naming rules and the boot-node
// decision it needs.
//
// Part of the generic autoboot/automount mounter; see LICENSE for the
// copyright and licence covering this file.
#include "mounter_internal.h"

// Buffer for a mount's DOS device name, sized like the RDB pb_DriveName[32]
// field it mirrors: a 32-byte BSTR holds a 31-char name (the RDB limit), and our
// NUL-terminated convention (length byte + chars + NUL) caps usable names at 30.
#define DEVNAME_BUFSIZE 32

/**
 * make_dos_name() - Seed a DOS device-name BSTR into the caller's buffer.
 * @dst:       buffer of @cap bytes; receives the length byte, the chars and a
 *             trailing NUL
 * @s:         the name as a C string, not NULL; cut to @cap - 2 chars, or to
 *             @cap - 3 when @seedDigit is set (room for the digit)
 * @cap:       size of @dst in bytes
 * @seedDigit: TRUE to append '0' unless the (cut) name already ends in a digit
 *
 * @seedDigit ensures a trailing digit ("UMSD" -> "UMSD0") so units number from
 * 0; a name already ending in a digit is left as given. That is what a recipe
 * name wants, because every volume the recipe mounts asks for the same one. An
 * RDB partition brings its own name from the disk and takes it verbatim —
 * seeding there would rename "Work" to "Work0".
 *
 * fix_name_collision() appends any collision digits later, in the same buffer.
 */
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

/**
 * to_upper() - Fold an ASCII lower-case letter to upper case.
 * @c: the char
 *
 * Return: the upper-case letter for 'a'..'z'; any other @c unchanged, Latin-1
 * letters included
 */
static UBYTE to_upper(UBYTE c)
{
	if (c >= 'a' && c <= 'z') {
		return c - ('a'-'A');
	}
	return c;
}

/**
 * bstr_equal_ci() - Compare a BSTR with @len chars, ignoring ASCII case.
 * @bstr:  the BSTR as a byte pointer (BADDR()'d): length byte, then the chars
 * @chars: the chars to compare against; need not be NUL-terminated
 * @len:   how many of @chars
 *
 * Callers holding a second BSTR pass its body and length byte; callers holding a
 * C string pass it with strlen(), which makes "same length" and "NUL lands at the
 * end" one test.
 *
 * Return: TRUE if @bstr's length byte equals @len and the chars match; FALSE
 * otherwise
 */
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

/**
 * is_same_extent() - Test whether a startup packet is our unit's, at this extent.
 * @md:      mount state: md->devicename and md->unitnum name the unit
 * @startup: a DeviceNode's dn_Startup; 0, a small integer or a BSTR allowed
 * @want:    the DosEnvec of the volume about to be mounted
 *
 * dn_Startup is a FileSysStartupMsg only for a filesystem: port-handler's SER:,
 * PAR: and PRT: keep an integer there, other handlers a startup string. So the
 * packet, its device name and its DosEnvec must all lie in RAM (TypeOfMem())
 * and de_TableSize must be plausible before anything in them is believed.
 *
 * The extent is the block size, the cylinder geometry and the cylinder range:
 * the same LowCyl/HighCyl under another geometry is another extent.
 *
 * Return: TRUE if this startup packet describes our device (md->devicename,
 * case-insensitive, its BSTR length counting the NUL or not), our unit and
 * exactly this extent; FALSE otherwise, including any @startup that is not a
 * FileSysStartupMsg or whose DosEnvec is too short to hold de_HighCyl
 */
static BOOL is_same_extent(struct MountData *md, BPTR startup, const struct DosEnvec *want)
{
	struct ExecBase *SysBase = md->SysBase;
	struct FileSysStartupMsg *fssm = (struct FileSysStartupMsg *)BADDR(startup);

	if ((ULONG)fssm < 0x1000 || !TypeOfMem(fssm) || fssm->fssm_Unit != md->unitnum) {
		return FALSE;
	}

	const UBYTE *dev = (const UBYTE *)BADDR(fssm->fssm_Device);
	const struct DosEnvec *de = (const struct DosEnvec *)BADDR(fssm->fssm_Environ);

	/* DE_UPPERCYL is the NDK's index name for the de_HighCyl field. */
	if (!TypeOfMem((APTR)dev) || !TypeOfMem((APTR)de) ||
	    de->de_TableSize < DE_UPPERCYL || de->de_TableSize > 64) {
		return FALSE;
	}

	if (de->de_SizeBlock != want->de_SizeBlock || de->de_Surfaces != want->de_Surfaces ||
	    de->de_SectorPerBlock != want->de_SectorPerBlock ||
	    de->de_BlocksPerTrack != want->de_BlocksPerTrack ||
	    de->de_LowCyl != want->de_LowCyl || de->de_HighCyl != want->de_HighCyl) {
		return FALSE;
	}
	// MakeDosNode() counts the terminating NUL in this BSTR's length
	// ("usbscsi.device" is 15 there), a node from Mount or a DOSDriver need
	// not: accept both, the + 1 comparing our own NUL against the counted one.
	const UWORD len = (UWORD)strlen((const char *)md->devicename);
	return bstr_equal_ci(dev, md->devicename, len) ||
	       bstr_equal_ci(dev, md->devicename, (UWORD)(len + 1));
}

/**
 * is_extent_mounted() - Look for an existing DeviceNode for this unit and extent.
 * @md:      mount state: device and unit as for is_same_extent(); md->DOSBase,
 *           when set, adds the DOS device list to the search
 * @want:    the DosEnvec of the volume about to be mounted
 *
 * Searches eb_MountList under Forbid(), then, with DOS, the device list under
 * LockDosList(LDF_DEVICES | LDF_READ).
 *
 * The pre-DOS pass mounts everything it can reach through FileSystem.resource,
 * and massstorage then asks for a re-probe once dos.library exists so that
 * partitions needing a *loadable* handler get their second chance. Without this
 * test that second pass re-mounts what already worked: fix_name_collision() finds
 * the name taken, renames CD0 to CD1, and a second handler opens the same medium
 * behind the one DOS is booting from — which surfaces mid-boot as "Please replace
 * volume <name> in any drive".
 *
 * Identity is device + unit + extent rather than the DOS name, so two different
 * drives that both want CD0 are still renamed apart exactly as before.
 *
 * Return: TRUE if a DeviceNode for exactly this device, unit and extent already
 * exists; FALSE if none does
 */
static BOOL is_extent_mounted(struct MountData *md, const struct DosEnvec *want)
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
		if (dn && is_same_extent(md, dn->dn_Startup, want)) {
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
			if (is_same_extent(md, (BPTR)dl->dol_misc.dol_handler.dol_Startup, want)) {
				found = TRUE;
				break;
			}
		}
		UnLockDosList(LDF_DEVICES | LDF_READ);
	}

	if (found) {
		dbg("Extent %lu..%lu on unit %lu already mounted\n", want->de_LowCyl, want->de_HighCyl,
		    md->unitnum);
	}
	return found;
}

/**
 * is_name_taken() - Check a DOS device name for duplicates.
 * @md:    mount state; md->DOSBase, when set, adds the DOS list to the search
 * @bname: the candidate as a NUL-terminated BSTR (length byte, chars, NUL), as
 *         make_dos_name() builds it
 *
 * Compares against the dn_Name of every eb_MountList node that has a DeviceNode
 * (ASCII case-insensitive) and, with DOS, asks FindDosEntry() over LDF_ALL.
 *
 * Return: TRUE if an eb_MountList node, or a DOS device, volume or assign,
 * already has the name; FALSE otherwise
 */
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
		const UBYTE *bname2 = dn ? BADDR(dn->dn_Name) : NULL;
		if (bname2 && bstr_equal_ci(bname, bname2 + 1, bname2[0])) {
			found = TRUE;
			break;
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

/**
 * bump_number() - Add one to the number a name ends in.
 * @name:   the name's chars, NUL-terminated
 * @len:    how many
 * @maxlen: the most chars the buffer holds, with room for a NUL after them
 *
 * Only the trailing run of digits counts. "UMSD0" -> "UMSD1" and "UMSD19" ->
 * "UMSD20" keep the length; "UMSD9" -> "UMSD10" grows by a digit, and so does a
 * name without a number: "Work" -> "Work1".
 *
 * Return: the new length; 0 when the name would have to grow past @maxlen, the
 * name then left as it was
 */
static int bump_number(UBYTE *name, int len, int maxlen)
{
	// The trailing 9s carry: find the last char that is not one.
	int pos = len - 1;
	while (pos >= 0 && name[pos] == '9')
		pos--;

	if (pos >= 0 && name[pos] >= '0' && name[pos] <= '8') {
		// A digit takes the carry: "UMSD19", the 1 becomes a 2.
		name[pos]++;
	} else {
		// None does ("UMSD9", or a name without a number): the number gains a
		// leading 1, where its first 9 was or at the end, and the name grows by one.
		if (len >= maxlen)
			return 0;
		name[++pos] = '1';
		name[++len] = 0;
	}
	// The digits after it, the 9s that carried, become 0s: "UMSD20", "UMSD10".
	for (int i = pos + 1; i < len; i++)
		name[i] = '0';
	return len;
}

/**
 * fix_name_collision() - Bump a DOS name's trailing number until it is unique.
 * @md:    mount state; md->renamed counts a name that had to be bumped, once
 * @bname: the name as a NUL-terminated BSTR (length byte, chars, trailing NUL),
 *         at most @cap - 2 chars as make_dos_name() leaves it; rewritten in place
 * @cap:   size of the buffer holding @bname, in bytes
 *
 * Bumps the name (bump_number()) until is_name_taken() says it collides with
 * nothing. Each bump gives a name not tried before, and the DOS list is finite,
 * so the loop ends; at the latest when the name can no longer grow.
 *
 * A collision is legitimate when another drive wants the same name, and is the
 * signature of a bug when it is the same volume being mounted twice (ZZ0 gaining
 * a live ZZ1). The two are told apart before we get here, by
 * is_extent_mounted(); every renamed volume is counted so the caller can report
 * it, because on a release build none of the logging below exists and this
 * failure is otherwise completely silent.
 *
 * Return: TRUE when @bname is a name nothing else has; FALSE when it is taken and
 * cannot grow, @bname then holding the last name tried
 */
static BOOL fix_name_collision(struct MountData *md, UBYTE *bname, int cap)
{
	BOOL bumped = FALSE;

	while (is_name_taken(md, bname)) {
		if (!bumped) {
			md->renamed++;
			bumped = TRUE;
		}
		dbg("Duplicate device name '%s'\n", bname + 1);
		int len = bump_number(bname + 1, bname[0], cap - 2);
		if (!len)
			return FALSE;
		bname[0] = (UBYTE)len;
		dbg("-> new device name '%s'\n", bname + 1);
	}
	return TRUE;
}

/**
 * make_fake_configdev() - Give a pre-DOS mount a ConfigDev, without which its
 * BootNodes are not bootable.
 * @md: mount state; md->configDev receives the ConfigDev, or NULL if
 *      AllocConfigDev() fails (every node is then added non-bootable)
 *
 * AddBootNode() branches on the ConfigDev pointer — expansion.doc: "Autoboot from
 * an expansion card before DOS is running requires the card's ConfigDev
 * structure.  Pass a NULL ConfigDev pointer to create a non-bootable node."  Only
 * a bootable node is NT_BOOTNODE, and only those are candidates for the boot
 * scan.  So a caller that mounts before DOS exists — a boot ROM, which is the
 * only way to get a BootNode at all — has to supply one even when there is no
 * real autoconfig board behind the drive.
 *
 * The ConfigDev gets ERTF_DIAGVALID and, in cd_Rom.er_Reserved0c, a pointer to
 * psd_diag_area (bootpoint.S), whose da_BootPoint is what strap calls to boot.
 *
 * Deliberately never freed — the BootNode's LN_NAME points at it for the life of
 * the machine.  AllocConfigDev() without AddConfigDev() also keeps it off
 * expansion's board list, where romboot scans diag areas for romtags.
 */
static void make_fake_configdev(struct MountData *md)
{
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	extern const struct DiagArea psd_diag_area;   // mounter/bootpoint.S

	md->configDev = AllocConfigDev();
	if (md->configDev) {
		md->configDev->cd_Rom.er_Type |= ERTF_DIAGVALID;
		*(APTR *)&md->configDev->cd_Rom.er_Reserved0c = (APTR)&psd_diag_area;
	}
	dbg("Fake ConfigDev for pre-DOS boot: 0x%08lx (DiagArea 0x%08lx)\n",
	    (ULONG)md->configDev, (ULONG)&psd_diag_area);
}

/**
 * add_mount_node() - Add a DeviceNode to the system.
 * @md:      mount state; md->DOSBase, md->flags and md->configDev decide whether
 *           the node is bootable
 * @bootPri: boot priority, bn_Node.ln_Pri; MOUNT_NEVER_BOOT never boots
 * @dn:      the node from MakeDosNode(), filesystem already attached; the system
 *           owns it once added
 *
 * AddBootNode() with ADNF_STARTPROC, and with a ConfigDev only pre-DOS (no
 * md->DOSBase), for a @bootPri above MOUNT_NEVER_BOOT and without MSF_NO_BOOT;
 * otherwise with a NULL ConfigDev. That ConfigDev is the caller's
 * (ms->configDev) or, made on the first bootable node of this MountDrive() call
 * and kept for the rest, a fake one (make_fake_configdev()); without one the
 * node still goes in, non-bootable.
 *
 * A BootNode is only NT_BOOTNODE — and so only reachable by strap — when a
 * ConfigDev is supplied, which is what makes this the pre-DOS boot path (see
 * make_fake_configdev()). Post-DOS, or for anything not meant to be booted, a
 * NULL ConfigDev is exactly what the pre-V36 AddDosNode() meant: expansion.doc
 * calls it "the old (pre V36) function that works just like AddBootNode()".
 * So one call covers both worlds.
 *
 * Return: AddBootNode()'s answer: TRUE if the node was added, FALSE if not (no
 * memory, or post-DOS a name AddDosEntry() refuses), when @dn stays the caller's
 */
static BOOL add_mount_node(struct MountData *md, LONG bootPri, struct DeviceNode *dn)
{
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	struct ConfigDev *cd = NULL;

	if (!md->DOSBase && bootPri > MOUNT_NEVER_BOOT && !(md->flags & MSF_NO_BOOT)) {
		if (!md->configDev)
			make_fake_configdev(md);
		cd = md->configDev;
	}
	dbg("Mounting %s: pri %ld\n", cd ? "bootable" : "non-bootable", bootPri);
	return AddBootNode(bootPri, ADNF_STARTPROC, dn, cd);
}

/**
 * envec_from_recipe() - Fill a recipe volume's DosEnvec from its blocks.
 * @md:  mount state: md->blocksize sets de_SizeBlock, md->dmaAlign
 *       de_BufMemType
 * @vol: the volume: fs (dosType, buffers and maxTransfer are used), firstBlock,
 *       lastBlock and bootPri; vol->pp.de must be zeroed, the fields not set here
 *       stay 0
 *
 * The blocks map to de_LowCyl..de_HighCyl at one block per "cylinder"
 * (Surfaces, SectorPerBlock and BlocksPerTrack are all 1); 0 and 0 is a
 * whole-medium device such as a CD. de_TableSize is 16 (through de_DosType);
 * mnt_mount_volume() raises it when the recipe brings a de_Control.
 */
static void envec_from_recipe(struct MountData *md, struct Volume *vol)
{
	const struct MountFS *fs = vol->fs;
	struct DosEnvec *de = &vol->pp.de;

	de->de_TableSize      = 16; // up to DE_DOSTYPE
	de->de_SizeBlock      = md->blocksize >> 2;
	de->de_Surfaces       = 1;
	de->de_SectorPerBlock = 1;
	de->de_BlocksPerTrack = 1;
	de->de_LowCyl         = vol->firstBlock;
	de->de_HighCyl        = vol->lastBlock;
	de->de_NumBuffers     = fs->buffers ? fs->buffers : 5;
	de->de_MaxTransfer    = fs->maxTransfer ? fs->maxTransfer : 0x100000;
	/* A device that reports a DMA alignment gets the filesystem's own buffers
	 * in Fast memory, out of Chip RAM. The alignment itself is not imposed
	 * through de_Mask: filesystems cannot honour it (PFS3 has no aligned
	 * allocator and raises a requester per buffer, FFS drops to 512-byte
	 * reads, fat95 to its cluster cache), and the device bounces misaligned
	 * or unreachable buffers itself. No MEMF_CLEAR: it is an allocation flag,
	 * not a memory property, so fat95's "caller buffer has every BufMemType
	 * bit" test would never pass. */
	de->de_BufMemType = (md->dmaAlign > 1) ? (MEMF_FAST|MEMF_PUBLIC) : MEMF_ANY;
	de->de_Mask       = 0x7FFFFFFE;
	de->de_DosType        = fs->dosType;
	de->de_BootPri        = vol->bootPri;
}

/**
 * mnt_mount_volume() - Create and add the DeviceNode for one volume.
 * @md:  mount state of the unit being scanned
 * @vol: the volume as its scanner filled it in: nameHint, bootPri, and either
 *       pp.de and fse (RDB; fse NULL for the ROM filesystem) or fs, firstBlock
 *       and lastBlock (recipe). For a recipe pp.de is built here, with
 *       de_Control and de_TableSize 19 when the recipe has a control string;
 *       pp.execname and pp.unitnum are set here, and pp.dosname for
 *       MakeDosNode() only, NULL again on return
 *
 * Every scanner ends here, so the duplicate guard, the filesystem choice, the
 * naming rules and the boot-node decision each exist exactly once.
 *
 * The order matters. The extent check comes first, the filesystem next, the name
 * last: a volume that is already mounted, or cannot be mounted, never reaches the
 * naming step, so md->renamed counts only volumes that get a node — the signal
 * it exists to give.
 *
 * A recipe's handler and control BSTRs are freed if no node takes them, so a
 * volume that gets no node leaks nothing. A node AddBootNode() refuses is lost
 * with what it holds: expansion.library offers no way to free a MakeDosNode()
 * result.
 *
 * Return: MOUNT_OK if the node was created, its filesystem attached and the node
 * added (md->mounted counts it); MOUNT_ALREADY if is_extent_mounted() found the
 * extent mounted and nothing was added (md->alreadyMounted and md->mounted count
 * it); MOUNT_DEFERRED if the recipe's filesystem is a handler file and there is
 * no DOS yet (md->deferred counts it); MOUNT_FAILED if no filesystem serves the
 * recipe, no unique DOS name could be made, or MakeDosNode() or AddBootNode()
 * failed
 */
enum MountOutcome mnt_mount_volume(struct MountData *md, struct Volume *vol)
{
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;   /* MakeDosNode base */
	struct FsChoice fc = { .fse = vol->fse };
	UBYTE name[DEVNAME_BUFSIZE];

	if (vol->fs)
		envec_from_recipe(md, vol);
	if (is_extent_mounted(md, &vol->pp.de)) {
		md->alreadyMounted++;
		md->mounted++;
		return MOUNT_ALREADY;
	}

	if (vol->fs) {
		enum MountOutcome outcome = mnt_resolve_fs(md, vol->fs, &fc);
		if (outcome != MOUNT_OK) {
			mnt_log("No filesystem for dostype 0x%08lx (%s) at block %lu%s\n", vol->fs->dosType,
			       vol->fs->handler ? (const char *)vol->fs->handler : (const char *)"no handler",
			       vol->firstBlock, outcome == MOUNT_DEFERRED ? ", until DOS is up" : "");
			if (outcome == MOUNT_DEFERRED)
				md->deferred++;
			return outcome;
		}
		if (fc.control) {
			vol->pp.de.de_Control   = (ULONG)MKBADDR(fc.control);
			vol->pp.de.de_TableSize = 19;   // up to de_BootBlocks, includes de_Control
		}
	}

	// A recipe name is shared by every volume that recipe mounts, so it carries a
	// unit digit; an RDB partition brings a name of its own and keeps it verbatim.
	make_dos_name(name, vol->nameHint, sizeof(name), vol->fs != NULL);
	if (!fix_name_collision(md, name, sizeof(name))) {
		mnt_log("No free DOS name for '%s'\n", (const char *)vol->nameHint);
		mnt_release_fs(md, &fc);
		return MOUNT_FAILED;
	}

	vol->pp.dosname  = name + 1;
	vol->pp.execname = md->devicename;
	vol->pp.unitnum  = md->unitnum;
	struct DeviceNode *dn = MakeDosNode(&vol->pp);
	vol->pp.dosname  = NULL;    // this frame's buffer; MakeDosNode() made its own copy
	if (!dn) {
		mnt_log("Could not create DosNode\n");
		mnt_release_fs(md, &fc);
		return MOUNT_FAILED;
	}
	mnt_attach_fs(dn, &fc);
	if (!add_mount_node(md, vol->bootPri, dn)) {
		mnt_log("Could not add DosNode '%s'\n", (const char *)(name + 1));
		return MOUNT_FAILED;
	}
	md->mounted++;
	return MOUNT_OK;
}
