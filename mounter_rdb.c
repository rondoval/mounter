
// mounter_rdb.c — RigidDiskBlock: PART chain, FSHD chain, and the
// LoadSeg-block hunk loader that brings a filesystem in off the disk.
//
// Part of the generic autoboot/automount mounter; see mounter.c for the
// copyright and licence covering this file.
#include "mounter_internal.h"

#define LSEG_DATASIZE (512 / 4 - 5)

// Hardening caps against corrupt/hostile on-disk metadata.
// The RDB PART and FSHD chains are singly linked by block number; a cyclic link
// would otherwise loop forever (the PART loop re-mounting each lap). The reloc
// caps keep the (size + 2) * 4 byte allocations below the 32-bit wrap point.
#define MAX_RDB_PARTITIONS 128          // realistic ceiling; > any sane RDB
#define MAX_RDB_FILESYS    64           // FSHD chain length
#define MAX_RELOC_HUNKS    4096         // hunks in one loaded filesystem
#define MAX_HUNK_LONGS     (16UL * 1024 * 1024 / sizeof(ULONG))  // 16 MB per hunk

// The RDB LoadSeg block chain, read as a stream of longs and words.
//
// Owned by parse_fshd() for the duration of one filesystem load and handed to
// fs_relocate(). It used to live in MountData, where every other scanner could see
// six fields that only the hunk loader has any business touching.
struct LSegStream
{
	struct MountData *md;           /* for mnt_read_block() */
	ULONG block;                    /* next LSEG block; 0xffffffff = end of chain */
	ULONG longs;                    /* longs still unread in buf */
	ULONG offset;                   /* read cursor into buf->lsb_LoadData */
	struct LoadSegBlock *buf;       /* one scratch sector */
	UWORD wordbuf;                  /* second half of a long, for lseg_read_word */
	UWORD hasword;
};

// Read multiple longs from LSEG blocks
static BOOL lseg_read_longs(struct LSegStream *ls, ULONG longs, ULONG *data)
{
	ULONG cnt = 0;
	ls->hasword = FALSE;
	while (longs > cnt) {
		if (ls->longs > 0) {
			data[cnt] = ls->buf->lsb_LoadData[ls->offset];
			ls->offset++;
			ls->longs--;
			cnt++;
			if (longs == cnt) {
				return TRUE;
			}
		}
		if (!ls->longs) {
			if (ls->block == 0xffffffff) {
				dbg("lseg_read_long premature end!\n");
				return FALSE;
			}
			if (!mnt_read_block((UBYTE*)ls->buf, ls->block, IDNAME_LOADSEG, ls->md)) {
				return FALSE;
			}
			ls->longs = LSEG_DATASIZE;
			ls->offset = 0;
			ls->block = ls->buf->lsb_Next;
		}
	}
	return TRUE;
}
// Read single long from LSEG blocks
static BOOL lseg_read_long(struct LSegStream *ls, ULONG *data)
{
	BOOL v;
	if (ls->hasword) {
		ULONG temp;
		v = lseg_read_longs(ls, 1, &temp);
		if (v) {
			*data = (ls->wordbuf << 16) | (temp >> 16);
			ls->wordbuf = (UWORD)temp;
		}
	} else {
		v = lseg_read_longs(ls, 1, data);
	}
	return v;
}
// Read single word from LSEG blocks
// Internally reads long and buffers second word.
static BOOL lseg_read_word(struct LSegStream *ls, ULONG *data)
{
	if (ls->hasword) {
		*data = ls->wordbuf;
		ls->hasword = FALSE;
		dbg("lseg_read_word 2/2 %08lx\n", *data);
		return TRUE;
	}
	ULONG temp;
	BOOL v = lseg_read_longs(ls, 1, &temp);
	if (v) {
		ls->hasword = TRUE;
		ls->wordbuf = (UWORD)temp;
		*data = temp >> 16;
	}
	dbg("lseg_read_word 1/2 %08lx\n", *data);
	return v;
}

struct RelocHunk
{
	ULONG hunkSize;
	ULONG *hunkData;
};

// Filesystem relocator
static APTR fs_relocate(struct LSegStream *ls)
{
	struct ExecBase *SysBase = ls->md->SysBase;
	ULONG data;
	struct RelocHunk *relocHunks;
	ULONG firstHunk, lastHunk;
	ULONG totalHunks;
	UWORD hunkCnt;
	WORD ret = 0;
	APTR firstProcessedHunk = NULL;

	if (!lseg_read_long(ls, &data)) {
		return NULL;
	}
	if (data != HUNK_HEADER) {
		return NULL;
	}
	// Read the size of a resident library name. This should
	// never be != 0.
	if (!lseg_read_long(ls, &data) || data != 0) {
		return NULL;
	}
	// Read the size of the hunk table, which should be > 0.
	// Note that this number may be larger than the
	// difference between the last and the first hunk + 1 for
	// overlay binary files. But then this function does not
	// support overlay binary files.
	if (!lseg_read_long(ls, &data) || data <= 0) {
		return NULL;
	}
	// first hunk
	if (!lseg_read_long(ls, &firstHunk)) {
		return NULL;
	}
	// last hunk
	if (!lseg_read_long(ls, &lastHunk)) {
		return NULL;
	}
	// Hunk numbers are non-negative on disk; the top bit set is corrupt data.
	// (They are read as raw longs, so this is the unsigned spelling of the old
	// firstHunk < 0 || lastHunk < 0 test.)
	if (((firstHunk | lastHunk) & 0x80000000UL) || firstHunk > lastHunk) {
		return NULL;
	}
	totalHunks = lastHunk - firstHunk + 1;
	// firstHunk/lastHunk are untrusted; a huge span would overflow the AllocMem
	// size below (and every later hunkCnt loop).
	if (totalHunks > MAX_RELOC_HUNKS) {
		dbg("Too many hunks (%ld)\n", totalHunks);
		return NULL;
	}
	dbg("first hunk %lu, last hunk %lu\n", firstHunk, lastHunk);
	relocHunks = AllocMem(totalHunks * sizeof(struct RelocHunk), MEMF_CLEAR);
	if (!relocHunks) {
		return NULL;
	}

	// Pre-allocate hunks
	ULONG *prevChunk = NULL;
	hunkCnt = 0;
	while (hunkCnt < totalHunks) {
		struct RelocHunk *rh = &relocHunks[hunkCnt];
		ULONG hunkHeadSize;
		ULONG memoryFlags = MEMF_PUBLIC;
		if (!lseg_read_long(ls, &hunkHeadSize)) {
			goto end;
		}
		if ((hunkHeadSize & (HUNKF_CHIP | HUNKF_FAST)) == (HUNKF_CHIP | HUNKF_FAST)) {
			if (!lseg_read_long(ls, &memoryFlags)) {
				goto end;
			}
		} else if (hunkHeadSize & HUNKF_CHIP) {
			memoryFlags |= MEMF_CHIP;
		}
		hunkHeadSize &= ~(HUNKF_CHIP | HUNKF_FAST);
		// Cap the per-hunk size so (hunkHeadSize + 2) * 4 cannot wrap the 32-bit
		// allocation (a masked size can still be up to 0x3FFFFFFF longs).
		if (hunkHeadSize > MAX_HUNK_LONGS) {
			dbg("Hunk too large (%lu longs)\n", hunkHeadSize);
			goto end;
		}
		rh->hunkSize = hunkHeadSize;
		rh->hunkData = AllocMem((hunkHeadSize + 2) * sizeof(ULONG), memoryFlags | MEMF_CLEAR);
		if (!rh->hunkData) {
			goto end;
		}
		dbg("hunk %ld: ptr 0x%08lx, size %ld, memory flags %08lx\n", hunkCnt + firstHunk, (ULONG)rh->hunkData, hunkHeadSize, memoryFlags);
		rh->hunkData[0] = rh->hunkSize + 2;
		rh->hunkData[1] = (ULONG)MKBADDR(prevChunk);
		prevChunk = &rh->hunkData[1];
		rh->hunkData += 2;

		if (!firstProcessedHunk) {
			firstProcessedHunk = (APTR)(rh->hunkData - 1);
		}
		hunkCnt++;
	}
	dbg("hunks allocated\n");

	// Load hunks/relocate
	hunkCnt = 0;
	struct RelocHunk *rh = NULL;
	while (hunkCnt <= totalHunks) {
		ULONG hunkType;
		if (!lseg_read_long(ls, &hunkType)) {
			if (hunkCnt >= totalHunks) {
				break;  // normal end
			}
			goto end;
		}
		dbg("HUNK %08lx\n", hunkType);
		switch(hunkType)
		{
			case HUNK_CODE:
			case HUNK_DATA:
			case HUNK_BSS:
			{
				ULONG hunkSize;
				if (hunkCnt >= totalHunks) {
					goto end;  // overflow
				}
				rh = &relocHunks[hunkCnt++];
				if (!lseg_read_long(ls, &hunkSize)) {
					goto end;
				}
				if (hunkSize > rh->hunkSize) {
					goto end;
				}
				if (hunkType != HUNK_BSS) {
					if (!lseg_read_longs(ls, hunkSize, rh->hunkData)) {
						goto end;
					}
				}
			}
			break;
			case HUNK_RELOC32:
			case HUNK_RELOC32SHORT:
			{
				ULONG relocCnt, relocHunk;
				if (rh == NULL) {
					goto end;
				}
				for (;;) {
					if (!lseg_read_long(ls, &relocCnt)) {
						goto end;
					}
					if (!relocCnt) {
						break;
					}
					if (!lseg_read_long(ls, &relocHunk)) {
						goto end;
					}
					relocHunk -= firstHunk;
					if (relocHunk >= totalHunks) {
						goto end;
					}
					dbg("HUNK_RELOC32: relocs %lu hunk %lu\n", relocCnt, relocHunk + firstHunk);
					struct RelocHunk *rhr = &relocHunks[relocHunk];
					while (relocCnt != 0) {
						ULONG relocOffset;
						if (hunkType == HUNK_RELOC32SHORT) {
							if (!lseg_read_word(ls, &relocOffset)) {
								goto end;
							}
						} else {
							if (!lseg_read_long(ls, &relocOffset)) {
								goto end;
							}
						}
						// Guard hunkSize == 0: (0 - 1) * 4 would wrap to a huge
						// bound and let any offset through into a zero-size hunk.
						if (rh->hunkSize == 0 ||
						    relocOffset > (rh->hunkSize - 1) * sizeof(ULONG)) {
							goto end;
						}
						UBYTE *hData = (UBYTE*)rh->hunkData + relocOffset;
						if (relocOffset & 1) {
							// Odd address, 68000/010 support.
							ULONG v = (hData[0] << 24) | (hData[1] << 16) | (hData[2] << 8) | (hData[3] << 0);
							v += (ULONG)rhr->hunkData;
							hData[0] = (UBYTE)(v >> 24);
							hData[1] = (UBYTE)(v >> 16);
							hData[2] = (UBYTE)(v >>  8);
							hData[3] = (UBYTE)(v >>  0);
						} else {
							*((ULONG*)hData) += (ULONG)rhr->hunkData;
						}
						relocCnt--;
					}
				}
			}
			break;
			case HUNK_END:
			// do nothing
			if (hunkCnt >= totalHunks) {
				ret = 1;  // normal end
				goto end;
			}
			break;
			default:
			dbg("Unexpected HUNK!\n");
			goto end;
		}
	}
	ret = 1;

end:
	if (!ret) {
		dbg("reloc failed\n");
		hunkCnt = 0;
		while (hunkCnt < totalHunks) {
			struct RelocHunk *victim = &relocHunks[hunkCnt];
			if (victim->hunkData) {
				FreeMem(victim->hunkData - 2, (victim->hunkSize + 2) * sizeof(ULONG));
			}
			hunkCnt++;
		}
		firstProcessedHunk = NULL;
	} else {
		CacheClearU();   // the relocated filesystem is about to be executed
		dbg("reloc ok, first hunk 0x%08lx\n", (ULONG)firstProcessedHunk);
	}

	FreeMem(relocHunks, totalHunks * sizeof(struct RelocHunk));

	return firstProcessedHunk;
}

// Build a FileSysEntry from an RDB FileSysHeaderBlock, unless FileSystem.resource
// already carries an entry for this dostype at the same version or newer.
//
// Returns a fresh, unregistered entry with no seglist yet — the caller loads and
// relocates the filesystem into it and then hands it to fse_register(). Returns NULL if
// the resource's existing entry is good enough, if the allocation fails, or if
// FileSystem.resource cannot be reached at all.
//
// Both the dostype and the version come out of the header block, so there is
// nothing for the caller to pass alongside it and nothing that can disagree.
static struct FileSysEntry *fse_from_fshb(struct FileSysHeaderBlock *fshb, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	const UBYTE *creator = md->creator ? md->creator : (const UBYTE *)"";
	struct FileSysEntry *fse = NULL;

	Forbid();
	if (OpenResource((CONST_STRPTR)FSRNAME)) {
		struct FileSysEntry *existing = mnt_find_filesystem(fshb->fhb_DosType, 0, SysBase);
		if (existing && existing->fse_Version >= fshb->fhb_Version) {
			dbg("FileSystem.resource: entry 0x%08lx for %08lx is version 0x%08lx >= 0x%08lx, keeping it\n",
			    (ULONG)existing, fshb->fhb_DosType, existing->fse_Version, fshb->fhb_Version);
		} else {
			fse = AllocMem(sizeof(struct FileSysEntry) + strlen((const char *)creator) + 1,
			               MEMF_PUBLIC | MEMF_CLEAR);
			if (fse) {
				ULONG patchFlags = fshb->fhb_PatchFlags;
				if (patchFlags & 0x0001)
					fse->fse_Type = fshb->fhb_Type;
				if (patchFlags & 0x0002)
					fse->fse_Task = fshb->fhb_Task;
				if (patchFlags & 0x0004)
					fse->fse_Lock = (BPTR)fshb->fhb_Lock;
				if (patchFlags & 0x0008)
					fse->fse_Handler = (BSTR)fshb->fhb_Handler;
				if (patchFlags & 0x0010)
					fse->fse_StackSize = fshb->fhb_StackSize;
				if (patchFlags & 0x0020)
					fse->fse_Priority = fshb->fhb_Priority;
				if (patchFlags & 0x0040)
					fse->fse_Startup = fshb->fhb_Startup;
				if (patchFlags & 0x0080)
					fse->fse_SegList = fshb->fhb_SegListBlocks;
				if (patchFlags & 0x0100)
					fse->fse_GlobalVec = fshb->fhb_GlobalVec;
				fse->fse_DosType = fshb->fhb_DosType;
				fse->fse_Version = fshb->fhb_Version;
				fse->fse_PatchFlags = patchFlags;
				strcpy((char *)(fse + 1), (const char *)creator);
				fse->fse_Node.ln_Name = (char *)(fse + 1);
				dbg("FileSystem.resource: new FileSysEntry 0x%08lx for %08lx from FSHD\n",
				    (ULONG)fse, fshb->fhb_DosType);
			}
		}
	}
	Permit();
	return fse;
}

// Add new FileSysEntry to FileSystem.resource, or free it if the filesystem
// load failed (fse_SegList == 0) or the resource can't be reached. Returns TRUE
// if the entry survived (added and still valid to read), FALSE if it was freed —
// the caller must drop its pointer in that case, or it dangles.
static BOOL fse_register(struct FileSysEntry *fse, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	if (fse->fse_SegList) {
		Forbid();
		struct FileSysResource *fsr = OpenResource((CONST_STRPTR)FSRNAME);
		if (fsr) {
			AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
			dbg("FileSysEntry 0x%08lx added to FileSystem.resource, dostype %08lx\n", (ULONG)fse, fse->fse_DosType);
			Permit();
			return TRUE;
		}
		Permit();
	}
	// Match the allocation in fse_from_fshb(): struct + creator string + NUL.
	const UBYTE *creator = md->creator ? md->creator : (const UBYTE *)"";
	dbg("FileSysEntry 0x%08lx freed, dostype %08lx\n", (ULONG)fse, fse->fse_DosType);
	FreeMem(fse, sizeof(struct FileSysEntry) + strlen((const char *)creator) + 1);
	return FALSE;
}

// Parse FileSystem Header Blocks, load and relocate filesystem if needed.
static struct FileSysEntry *parse_fshd(ULONG block, ULONG dostype, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct FileSysEntry *fse = NULL;
	// The FileSysHeaderBlock and its LoadSegBlock chain live in two scratch
	// sectors that must coexist; both are released before returning.
	UBYTE *buf = mnt_sector_take(md);
	UBYTE *segbuf = mnt_sector_take(md);

	if (buf && segbuf) {
		struct FileSysHeaderBlock *fshb = (struct FileSysHeaderBlock*)buf;
		// cap the fhb_Next chain so a cyclic/corrupt RDB can't spin.
		for (int i = 0; i < MAX_RDB_FILESYS; i++) {
			if (block == 0xffffffff) {
				break;
			}
			if (!mnt_read_block(buf, block, IDNAME_FILESYSHEADER, md)) {
				break;
			}
			dbg("FSHD found, block %lu, dostype %08lx, looking for dostype %08lx\n", block, fshb->fhb_DosType, dostype);
			if (fshb->fhb_DosType == dostype) {
				dbg("FSHD dostype match found\n");
				fse = fse_from_fshb(fshb, md);
				if (fse) {
					struct LSegStream ls = {
						.md    = md,
						.block = (ULONG)fshb->fhb_SegListBlocks,
						.buf   = (struct LoadSegBlock *)segbuf,
					};
					APTR seg = fs_relocate(&ls);
					fse->fse_SegList = MKBADDR(seg);
					// Add to FileSystem.resource if succeeded, delete entry if
					// failure. On failure fse_register frees fse, so drop our pointer
					// (the caller must not read a dangling FileSysEntry).
					if (!fse_register(fse, md))
						fse = NULL;
				}
				break;
			}
			block = fshb->fhb_Next;
		}
	}
	// No FSHD on this disk carried the dostype (or loading it failed): fall back to
	// whatever FileSystem.resource already has for it, which may be nothing.
	if (!fse) {
		fse = mnt_find_filesystem(dostype, 0, SysBase);
	}
	if (segbuf) mnt_sector_drop(md, segbuf);
	if (buf)    mnt_sector_drop(md, buf);
	return fse;
}

// Parse one PART block and mount what it describes. Returns the next PART block in
// the chain; what was mounted lands in md->mounted.
static ULONG parse_part(UBYTE *buf, ULONG block, ULONG filesysblock, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct PartitionBlock *part = (struct PartitionBlock*)buf;
	ULONG nextpartblock = 0xffffffff;

	if (!mnt_read_block(buf, block, IDNAME_PARTITION, md)) {
		return nextpartblock;
	}
	dbg("PART found, block %lu\n", block);
	nextpartblock = part->pb_Next;
	if (part->pb_Flags & PBFF_NOMOUNT) {
		return nextpartblock;
	}

	// Heap rather than stack: this is the deepest point of the whole mounter, and
	// a Volume carries a DosEnvec.
	struct Volume *vol = AllocMem(sizeof(struct Volume), MEMF_PUBLIC | MEMF_CLEAR);
	if (!vol) {
		return nextpartblock;
	}

	// pb_Environment[0] (de_TableSize) is untrusted disk data. Clamp it to what
	// vol->pp.de (a struct DosEnvec) holds before copying, or the copy overruns
	// the ParameterPacket — and clamp de_TableSize itself so MakeDosNode, which
	// reads (de_TableSize + 1) longs, stays in bounds.
	ULONG tablesize = part->pb_Environment[0];
	if (tablesize > sizeof(struct DosEnvec) / sizeof(ULONG) - 1)
		tablesize = sizeof(struct DosEnvec) / sizeof(ULONG) - 1;
	CopyMem(&part->pb_Environment, &vol->pp.de, (tablesize + 1) * sizeof(ULONG));
	vol->pp.de.de_TableSize = tablesize;

	// The filesystem comes off this disk (or FileSystem.resource); no recipe is
	// involved, so vol->fs stays NULL and mnt_mount_volume() uses vol->fse.
	vol->fse = parse_fshd(filesysblock, vol->pp.de.de_DosType, md);

	// The RDB length byte is untrusted disk data. Clamp both it and the content
	// to what the field holds (length byte + chars + NUL) so every downstream
	// BSTR/C-string view of the name agrees on the length.
	UBYTE len = *part->pb_DriveName;
	if (len > (int)sizeof(part->pb_DriveName) - 2)
		len = (int)sizeof(part->pb_DriveName) - 2;
	part->pb_DriveName[0] = len;
	part->pb_DriveName[len + 1] = 0;
	vol->nameHint = part->pb_DriveName + 1;
	dbg("PART '%s'\n", vol->nameHint);

	// Only an RDB partition can actually be booted from, and only if its own RDB
	// says so. Note de_BootPri keeps whatever the disk said either way — the early
	// boot menu reads that, while strap orders by what we pass to AddBootNode().
	vol->bootPri = (part->pb_Flags & PBFF_BOOTABLE) ? vol->pp.de.de_BootPri : MOUNT_NEVER_BOOT;
	if (md->flags & MSF_NO_BOOT)
		vol->bootPri = MOUNT_NEVER_BOOT;

	mnt_mount_volume(md, vol);

	FreeMem(vol, sizeof(struct Volume));
	return nextpartblock;
}

// Walk the PART chain, mounting as we go. What was mounted lands in md->mounted.
static void parse_rdsk(UBYTE *buf, struct MountData *md)
{
	struct RigidDiskBlock *rdb = (struct RigidDiskBlock*)buf;
	ULONG partblock = rdb->rdb_PartitionList;
	ULONG filesysblock = rdb->rdb_FileSysHeaderList;
	ULONG flags = rdb->rdb_Flags;
	// a cyclic pb_Next on a corrupt RDB would otherwise re-mount
	// the same partitions forever. parse_part also returns 0xffffffff on read
	// failure, which ends the walk early.
	for (int i = 0; i < MAX_RDB_PARTITIONS; i++) {
		if (partblock == 0xffffffff) {
			break;
		}
		partblock = parse_part(buf, partblock, filesysblock, md);
	}

	md->wasLastDev = (flags & RDBFF_LAST) != 0;
}

// Search for an RDB and, if there is one, mount its partitions.
// Returns TRUE if this medium was recognized as RDB; the count is in md->mounted.
BOOL mnt_scan_rdb(struct MountData *md)
{
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return FALSE;
	BOOL found = FALSE;
	for (UWORD i = 0; i < RDB_LOCATION_LIMIT; i++) {
		if (mnt_read_block(buf, i, 0xffffffff, md)) {
			struct RigidDiskBlock *rdb = (struct RigidDiskBlock*)buf;
			if (rdb->rdb_ID == IDNAME_RIGIDDISK) {
				dbg("RDB found, block %lu\n", i);
				parse_rdsk(buf, md);
				found = TRUE;
				break;
			}
		}
	}
	mnt_sector_drop(md, buf);
	return found;
}
