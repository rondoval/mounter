
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

/**
 * bstr_alloc() - Copy a C string into a freshly allocated BCPL string.
 * @s:       NUL-terminated C string; only its first 255 chars are kept, the most
 *           a BSTR's single length byte can count
 * @SysBase: exec
 *
 * The result is the length byte, the chars and a trailing NUL (from MEMF_CLEAR),
 * so it doubles as a C string. Used for dn_Handler / de_Control, which the
 * DeviceNode owns for its lifetime once added — so only a BSTR no node took is
 * freed (bstr_free()).
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
 * bstr_free() - Free a BSTR from bstr_alloc() that no DeviceNode took.
 * @b:       the BSTR as a byte pointer; NULL accepted
 * @SysBase: exec
 */
static void bstr_free(UBYTE *b, struct ExecBase *SysBase)
{
	if (b)
		FreeMem(b, (ULONG)b[0] + 2);
}

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
 * read_geometry() - Ask the unit for its geometry with TD_GETGEOMETRY.
 * @md:       mount state; only md->SysBase is used
 * @req:      I/O request with the unit open on it
 * @geometry: receives the unit's DriveGeometry
 *
 * The caller takes the unit's block size (dg_SectorSize), dg_TotalSectors and
 * dg_DeviceType from it.
 *
 * Return: 0 on success; else the io_Error DoIO() returns
 */
static BYTE read_geometry(struct MountData *md, struct IOExtTD *req, struct DriveGeometry *geometry)
{
	struct ExecBase *SysBase = md->SysBase;

	req->iotd_Req.io_Command = TD_GETGEOMETRY;
	req->iotd_Req.io_Data    = geometry;
	req->iotd_Req.io_Length  = sizeof(struct DriveGeometry);

	return DoIO((struct IORequest *)req);
}

/**
 * block_checksum() - Verify the checksum of an RDB block (RDSK, PART, FSHD, LSEG).
 * @buf: the block as read, md->blocksize bytes; longword 1 holds its
 *       summed-longs count (rdb_SummedLongs, pb_SummedLongs, ...)
 * @md:  mount state; md->blocksize bounds the count
 *
 * The block is valid when its first summed-longs longwords, read big-endian,
 * add up to 0 (mod 2^32). The count has to cover at least the ID, the count and
 * the checksum longs: a smaller one would check nothing.
 *
 * Return: TRUE if they do; FALSE on a nonzero sum, or a count below 3 or larger
 * than the block (md->blocksize / 4)
 */
static BOOL block_checksum(UBYTE *buf, struct MountData *md)
{
	ULONG chk = 0;
	ULONG num_longs = ((ULONG)buf[4] << 24) | ((ULONG)buf[5] << 16) | ((ULONG)buf[6] << 8) | buf[7];
	// A block's summed-longs count can never exceed the block itself; reject
	// garbage counts so the checksum loop can't read past the sector buffer.
	if (num_longs < 3 || num_longs > md->blocksize / sizeof(LONG))
		return FALSE;

	for (ULONG i = 0; i < num_longs * sizeof(LONG); i += 4) {
		ULONG v = ((ULONG)buf[i] << 24) | ((ULONG)buf[i + 1] << 16) | ((ULONG)buf[i + 2] << 8) | buf[i + 3];
		chk += v;
	}
	if (chk) {
		dbg("Checksum error %08lx\n", chk);
		return FALSE;
	}
	return TRUE;
}


#define MAX_RETRIES 3

/**
 * mnt_read_block() - Read one block of the unit being scanned, with retries.
 * @buf:   destination, at least md->blocksize bytes
 * @block: block number, in md->blocksize units; past 2^32 bytes the read is
 *         TD_READ64
 * @id:    expected first longword (IDNAME_RIGIDDISK, IDNAME_PARTITION, ...),
 *         which also turns on the block_checksum() test; 0xffffffff for a raw
 *         read with neither test
 * @md:    mount state: md->request (the open unit), md->blocksize, md->flags
 *
 * Tries DoIO() up to MAX_RETRIES times, 15 with MSF_SLOW_SPINUP, with no delay
 * between attempts.
 *
 * Return: TRUE if the read succeeded and, for an @id other than 0xffffffff, the
 * block carries @id and a valid checksum; FALSE otherwise
 */
BOOL mnt_read_block(UBYTE *buf, uint64_t block, ULONG id, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct IOExtTD *request = md->request;
	UWORD i, max_retries = MAX_RETRIES;
	if (md->flags & MSF_SLOW_SPINUP)
		max_retries = 15;

	// Byte offsets past 4GB need TD_READ64 (io_Actual = high 32 bits)
	uint64_t offset = block * md->blocksize;
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
	ULONG v = ((ULONG)buf[0] << 24) | ((ULONG)buf[1] << 16) | ((ULONG)buf[2] << 8) | buf[3];
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

/**
 * mnt_sector_take() - Borrow a scratch sector for the duration of one scan step.
 * @md: mount state holding the pool
 *
 * The scanners nest — block 0, then a GPT header, then the entry array, then a
 * partition's VBR — and each wants a MAX_BLOCKSIZE buffer. They used to AllocMem
 * their own, which meant eight allocation sites, eight out-of-memory paths, and
 * two allocations *per partition* in the RDB loop. Use is strictly LIFO (every
 * buffer is released with mnt_sector_drop() before its taker returns), so a
 * stack of SECTOR_SLOTS slots is enough.
 *
 * Slots are allocated on first use and live until mnt_sector_pool_free(), when
 * MountDrive() (or mnt_find_containers()) returns, so a plain FAT stick never
 * allocates the GPT path's buffers and a 10-partition RDB allocates its FSHD
 * sectors once instead of twenty times.
 *
 * Return: a MAX_BLOCKSIZE-byte MEMF_PUBLIC buffer, not cleared; NULL if all
 * SECTOR_SLOTS are taken or AllocMem() fails, and nothing is taken
 */
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

/**
 * mnt_sector_drop() - Release the most recently taken sector.
 * @md:  mount state holding the pool
 * @buf: the buffer the latest mnt_sector_take() returned
 *
 * The buffer stays allocated in its slot for the next taker. Anything but the
 * current top is a programming error: report it and leave the stack alone
 * rather than corrupt it.
 */
void mnt_sector_drop(struct MountData *md, const UBYTE *buf)
{
	if (!md->sectorsUsed || md->sector[md->sectorsUsed - 1] != buf) {
		printf("Sector pool released out of order\n");
		return;
	}
	md->sectorsUsed--;
}

/**
 * mnt_sector_pool_free() - Free every slot of the scratch sector pool.
 * @md: mount state holding the pool; the MountData itself stays allocated
 *
 * FreeMem()s each allocated slot and resets the stack to empty, so it must not
 * run while a sector is still taken. MountDrive() and mnt_find_containers()
 * call it before freeing their MountData.
 */
void mnt_sector_pool_free(struct MountData *md)
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
 * Return: the matching FileSysEntry; NULL if none matches or there is no
 * FileSystem.resource
 */
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
 * fix_name_collision() - Bump a DOS name's trailing number until it is unique.
 * @md:    mount state; md->renamed counts a name that had to be bumped, once
 * @bname: the name as a NUL-terminated BSTR (length byte, chars, trailing NUL),
 *         rewritten in place
 * @cap:   size of the buffer holding @bname, in bytes
 *
 * Bumps the trailing decimal number ("UMSD0" -> "UMSD1", "UMSD9" -> "UMSD10")
 * until is_name_taken() says the name collides with nothing. A name without a
 * trailing number (RDB partition names) gets "1" appended on its first
 * collision.
 *
 * The longest name the buffer can hold is @cap - 2 chars (one byte for the
 * length prefix, one for the NUL). Every write below is bounded by that limit:
 * the tail digit lands at most at name[maxlen-1], and a grown name plus its NUL
 * occupy through name[maxlen] == bname[cap-1], the last byte. A carry that would
 * have to grow a full-length name gives up instead of looping forever.
 *
 * A collision is legitimate when another drive wants the same name, and is the
 * signature of a bug when it is the same volume being mounted twice (ZZ0 gaining
 * a live ZZ1). The two are told apart before we get here, by
 * is_extent_mounted(); every renamed volume is counted so the caller can report
 * it, because on a release build none of the logging below exists and this
 * failure is otherwise completely silent.
 *
 * Return: TRUE when @bname is a name nothing else has; FALSE when it is taken and
 * cannot grow, @bname then holding a bumped but unchecked name
 */
static BOOL fix_name_collision(struct MountData *md, UBYTE *bname, int cap)
{
	int maxlen = cap - 2;
	BOOL bumped = FALSE;
	while (is_name_taken(md, bname)) {
		UBYTE len = bname[0] > maxlen ? (UBYTE)maxlen : bname[0];
		UBYTE *name = bname + 1;
		if (!bumped) {
			md->renamed++;
			bumped = TRUE;
		}
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
			return FALSE;
		}
		dbg("-> new device name '%s'\n", name);
	}
	return TRUE;
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
 * apply_patch_flags() - Copy a FileSystem.resource entry's fields into a DeviceNode.
 * @dn:  the node to patch
 * @fse: the entry; its fse_PatchFlags select the fields
 *
 * Bits 0-8 of fse_PatchFlags copy, in that order, fse_Type, fse_Task, fse_Lock,
 * fse_Handler, fse_StackSize, fse_Priority, fse_Startup, fse_SegList and
 * fse_GlobalVec to the matching dn_ field.
 */
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

/**
 * is_fs_available() - Test whether a recipe resolves to a working filesystem *now*.
 * @md: mount state; a NULL md->DOSBase means pre-DOS
 * @fs: the recipe; fs->dosType and fs->handler are used
 *
 * Checked before creating a DeviceNode so unresolvable partitions are skipped
 * clean, rather than leaving a node that fails the moment something touches it
 * (a handler the user never installed).
 *
 * "Now" is the load-bearing word before DOS exists. A handler path is a promise
 * only DOS can keep, so pre-DOS the answer is FALSE even for a recipe that names
 * one: the volume is not unmountable, it is not mountable *yet*. Saying TRUE here
 * used to leave a dead DeviceNode holding the DOS name, so the post-DOS pass that
 * could have mounted the volume properly found the name taken, renamed it, and
 * mounted a second node beside the dead one (ZZ0 gaining a ZZ1). The caller
 * counts a FALSE from here as deferred and comes back once DOS is up.
 *
 * The file check itself needs DOS and a Process (Lock() is a packet), so a
 * task-context caller that does have DOS still takes the handler on trust.
 *
 * Return: TRUE if the dostype is registered in FileSystem.resource, or DOS is
 * open and fs->handler is given and Lock() finds it (taken on trust from a
 * Task); FALSE if there is no resource entry and no handler, no resource entry
 * pre-DOS, or a handler Lock() cannot find (requesters suppressed)
 */
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

// What a recipe volume's DeviceNode will run: a FileSystem.resource entry, or a
// handler file by name (a BSTR the node takes over once it is added).
struct FsChoice
{
	struct FileSysEntry *fse;
	UBYTE *handler;
};

/**
 * choose_fs() - Decide what a recipe volume's DeviceNode will run.
 * @md: mount state; a NULL md->DOSBase means pre-DOS
 * @fs: the volume's recipe
 * @fc: out: the choice; fc->handler, when set, is the caller's to bstr_free()
 *      unless attach_fs() hands it to a node
 *
 * The FileSystem.resource entry if the dostype is registered, else the recipe's
 * handler file (loaded by DOS on first access). MOUNTFS_FORCELOAD swaps that
 * order for recipes that must not lose to a stale controller-ROM filesystem
 * sitting on their dostype (what ForceLoad=1 does in a mountlist), keeping the
 * resource entry as the fallback for machines that carry the handler in ROM and
 * nowhere else. Decided, and the handler BSTR allocated, before MakeDosNode(), so
 * a failure leaves no node behind.
 *
 * Not pre-DOS, though. There a handler path is a promise nobody can keep: the
 * node would carry "L:ODFileSystem" with no DOS to load it, so strap cannot boot
 * the volume however good the handler would have been. The resource entry is the
 * only thing that can work before DOS exists, so it wins there — which is the
 * whole point of putting a filesystem in the Kickstart in the first place.
 *
 * And if there is no resource entry, pre-DOS this must fail rather than fall back
 * to the handler path: a node that cannot be serviced still claims the DOS name,
 * which is exactly what turned the later, working mount into a renamed duplicate.
 * is_fs_available() already refuses such a recipe before we get here; this
 * stays consistent with it so the two cannot drift apart.
 *
 * Return: TRUE if a resource entry or the handler path was chosen; FALSE if there
 * is no resource entry and the handler path cannot be used (none given, pre-DOS,
 * or no memory for its BSTR)
 */
static BOOL choose_fs(struct MountData *md, const struct MountFS *fs, struct FsChoice *fc)
{
	struct ExecBase *SysBase = md->SysBase;
	BOOL forceLoad = md->DOSBase && fs->handler && (fs->fsFlags & MOUNTFS_FORCELOAD);

	fc->fse = mnt_find_filesystem(fs->dosType, 0, SysBase);
	fc->handler = NULL;
	if (fc->fse && !forceLoad)
		return TRUE;
	if (fs->handler && md->DOSBase) {
		fc->handler = bstr_alloc(fs->handler, SysBase);
		if (fc->handler)
			return TRUE;
	}
	return fc->fse != NULL;
}

/**
 * attach_fs() - Attach a choose_fs() choice to a fresh DeviceNode.
 * @dn: the node from MakeDosNode(), not yet added
 * @fs: the volume's recipe; fs->stackSize is used
 * @fc: the choice: the handler path wins when set (dn_Handler, dn_GlobalVec -1,
 *      dn_StackSize fs->stackSize or 8192), else the resource entry goes in
 *      through apply_patch_flags()
 */
static void attach_fs(struct DeviceNode *dn, const struct MountFS *fs, const struct FsChoice *fc)
{
	if (fc->handler) {
		dn->dn_Handler = MKBADDR(fc->handler);
		dn->dn_GlobalVec = (BPTR)-1;   /* C handler convention */
		dn->dn_StackSize = fs->stackSize ? fs->stackSize : 8192;
	} else {
		apply_patch_flags(dn, fc->fse);
	}
}

/**
 * mnt_envec_from_recipe() - Fill a DosEnvec for a recipe-mounted volume from a
 * block extent.
 * @md:      mount state: md->blocksize sets de_SizeBlock, md->dmaAlign
 *           de_BufMemType
 * @de:      the DosEnvec to fill; must be zeroed, the fields not set here stay 0
 * @fs:      the recipe: dosType, buffers and maxTransfer are used
 * @lowCyl:  first block of the volume, in md->blocksize units
 * @highCyl: last block of the volume, inclusive
 * @bootPri: de_BootPri
 *
 * @lowCyl/@highCyl bound the volume in blocks (Surfaces/SectorPerBlock/
 * BlocksPerTrack are all 1, so one block is one "cylinder"); pass 0/0 for a
 * whole-medium device such as a CD. The RDB path does not use this — its
 * DosEnvec comes off the disk.
 *
 * de_TableSize is 16 (through de_DosType); mnt_mount_volume() adds the recipe's
 * de_Control once it knows the volume gets a node.
 */
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
	de->de_BootPri        = bootPri;
}

/**
 * mnt_resolve_fs() - Decide whether a recipe can be mounted right now.
 * @md:     mount state; md->deferred counts recipes waiting on DOS
 * @fs:     the recipe
 * @lowCyl: first block of the volume; only logged
 *
 * A recipe that names a handler file is not unmountable when there is no DOS to
 * load it — it is unmountable *yet*, so it is counted as deferred and the caller
 * comes back once DOS is up. Mounting a placeholder node instead would claim the
 * DOS name and force that later, working mount into a renamed duplicate.
 *
 * Return: TRUE if is_fs_available() says this recipe can be mounted right now;
 * FALSE otherwise, logged, and counted in md->deferred when there is no DOS and
 * fs->handler is set
 */
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

/**
 * mnt_mount_volume() - Create and add the DeviceNode for one volume.
 * @md:  mount state of the unit being scanned
 * @vol: the volume as its scanner filled it in: pp.de, nameHint, bootPri, and fs
 *       (recipe) or fse (RDB), both NULL for an RDB partition on the ROM
 *       filesystem. pp.execname and pp.unitnum are set here, and for a recipe
 *       de_Control and de_TableSize 19 when it has a control string; pp.dosname
 *       is set for MakeDosNode() only and NULL again on return
 *
 * Every scanner ends here, so the duplicate guard, the naming rules, the
 * filesystem attach and the boot-node decision each exist exactly once.
 *
 * The extent check must stay first: a volume already mounted on an earlier pass
 * must never reach the naming step. The other order bumped the DOS name — and
 * md->renamed with it — for a volume the mounter then declined to mount, which
 * is exactly the signal md->renamed exists to give.
 *
 * A recipe's handler and control BSTRs are allocated before MakeDosNode() and
 * freed if it fails, so a volume that gets no node leaks nothing. A node
 * AddBootNode() refuses is lost with what it holds: expansion.library offers no
 * way to free a MakeDosNode() result.
 *
 * Increments md->mounted for anything that leaves a live node behind, so no caller
 * has to remember to count.
 *
 * Return: MOUNT_OK if the node was created, its filesystem attached and the node
 * added (md->mounted counts it); MOUNT_ALREADY if is_extent_mounted() found the
 * extent mounted and nothing was added (md->alreadyMounted and md->mounted count
 * it); MOUNT_FAILED if no unique DOS name could be made, the recipe's filesystem
 * cannot be attached (choose_fs()), or MakeDosNode() or AddBootNode() failed
 */
enum MountOutcome mnt_mount_volume(struct MountData *md, struct Volume *vol)
{
	struct ExecBase *SysBase = md->SysBase;
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;   /* MakeDosNode base */
	UBYTE name[DEVNAME_BUFSIZE];

	if (is_extent_mounted(md, &vol->pp.de)) {
		md->alreadyMounted++;
		md->mounted++;
		return MOUNT_ALREADY;
	}

	// A recipe name is shared by every volume that recipe mounts, so it carries a
	// unit digit; an RDB partition brings a name of its own and keeps it verbatim.
	make_dos_name(name, vol->nameHint, sizeof(name), vol->fs != NULL);
	if (!fix_name_collision(md, name, sizeof(name))) {
		printf("No free DOS name for '%s'\n", (const char *)vol->nameHint);
		return MOUNT_FAILED;
	}

	// Recipe: the handler has to be resolvable or the node is dead weight.
	struct FsChoice fc = { NULL, NULL };
	UBYTE *control = NULL;
	if (vol->fs) {
		if (!choose_fs(md, vol->fs, &fc)) {
			printf("Could not load filesystem\n");
			return MOUNT_FAILED;
		}
		// A failed control BSTR only leaves the volume without de_Control.
		if (vol->fs->control && (control = bstr_alloc(vol->fs->control, SysBase))) {
			vol->pp.de.de_Control   = (ULONG)MKBADDR(control);
			vol->pp.de.de_TableSize = 19;   // up to de_BootBlocks, includes de_Control
		}
	}

	vol->pp.dosname  = name + 1;
	vol->pp.execname = md->devicename;
	vol->pp.unitnum  = md->unitnum;
	struct DeviceNode *dn = MakeDosNode(&vol->pp);
	vol->pp.dosname  = NULL;    // this frame's buffer; MakeDosNode() made its own copy
	if (!dn) {
		printf("Could not create DosNode\n");
		bstr_free(control, SysBase);
		bstr_free(fc.handler, SysBase);
		return MOUNT_FAILED;
	}
	if (vol->fs) {
		attach_fs(dn, vol->fs, &fc);
	} else if (vol->fse) {
		// RDB: the filesystem came off the disk, or from FileSystem.resource.
		apply_patch_flags(dn, vol->fse);
	}
	if (!add_mount_node(md, vol->bootPri, dn)) {
		printf("Could not add DosNode '%s'\n", (const char *)(name + 1));
		return MOUNT_FAILED;
	}
	md->mounted++;
	return MOUNT_OK;
}

/**
 * probe_unit() - Open one unit, scan its medium and close it again.
 * @md:      mount state; its per-unit fields (request, devicename, blocksize,
 *           totalsectors, unitnum, legacyMounted, mounted) are set here
 * @ms:      the caller's request; ms->deviceName is the device opened
 * @unitNum: unit to open
 * @request: the caller's I/O request, not open; opened on @unitNum here and
 *           closed again before return
 *
 * The scanners answer one question each — did you recognize this medium? — and
 * leave the count in md->mounted, so the -1/0/count encoding is applied here,
 * once. A direct-access unit goes to mnt_scan_rdb(), then to mnt_scan_legacy()
 * if that recognized nothing; a CD-ROM, WORM or optical unit to mnt_scan_cd().
 * MSF_NO_RDB, MSF_NO_LEGACY and MSF_NO_CD turn the scanners off.
 *
 * Return: -1 if nothing recognized the medium, including when OpenDevice() or
 * TD_GETGEOMETRY fails, dg_SectorSize is outside 256..MAX_BLOCKSIZE or no
 * scanner takes the device type; else the number mounted (md->mounted, extents
 * found already mounted included; 0 = recognized but nothing mounted)
 */
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

/**
 * scan_units() - Probe each unit the caller listed, in order.
 * @md:         mount state, shared by all the units
 * @ms:         the caller's request: ms->units[], ms->unitCount, and
 *              ms->unitResults (NULL if not wanted)
 * @request:    I/O request for probe_unit(), reused for every unit
 * @total:      in/out; each recognized unit's mount count is added to it
 * @recognized: out; set TRUE when any unit's medium is recognized, never cleared
 *
 * Explicit units let a caller mount known units (e.g. a hotplug driver) instead
 * of scanning. Each ms->unitResults[i] gets that unit's probe_unit() result, or
 * -2 when the unit was skipped after a prior RDBFF_LAST (ignored with
 * MSF_IGNORE_LAST).
 */
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

/**
 * MountDrive() - Mount the volumes on the given units of one device.
 * @ms:  what to mount: device, units, flags, recipes; never written, except
 *       through ms->unitResults (-1/0/count per unit, -2 = skipped because a
 *       previous unit had RDBFF_LAST set)
 * @res: zeroed first, then filled with the per-call breakdown; NULL if not wanted
 *
 * The one way in; struct MountStruct and struct MountResult in mounter.h describe
 * the fields. md->DOSBase stays NULL while dos.library (V40) cannot be opened,
 * and that is what selects the pre-DOS paths throughout.
 *
 * Return: >0, the volumes mounted across all units (extents found already
 * mounted included); 0 if a unit's medium was recognized but nothing mounted;
 * -1 if nothing was recognized on any unit, no units were given, or setup failed
 * (expansion.library V40, memory, message port or I/O request)
 */
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

	// Before DOS exists a bootable node needs a ConfigDev to be an NT_BOOTNODE at
	// all; add_mount_node() makes a fake one on the first such node unless the
	// caller has a real board (ms->configDev).

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
		mnt_sector_pool_free(md);
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
