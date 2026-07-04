
// Generic autoboot/automount RDB parser and mounter.
// - KS 1.3 support, including autoboot mode.
// - 68000 compatible.
// - Boot ROM and executable modes.
// - Autoboot capable (Boot ROM mode only).
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
#ifdef DEBUG_MOUNTER
#define USE_SERIAL_OUTPUT
#endif

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/alerts.h>
#include <exec/ports.h>
#include <exec/execbase.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/hardblocks.h>
#include <devices/scsidisk.h>
#include <resources/filesysres.h>
#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <libraries/configvars.h>
#include <clib/alib_protos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/doshunks.h>

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>

#include "ndkcompat.h"

#include "mounter.h"
#include "legacy.h"

#ifndef SID_TYPE
#define SID_TYPE 0x1F
#endif

#ifndef HD_WIDESCSI
#define HD_WIDESCSI 8
#endif

#define TRACE 1
#undef TRACE_LSEG
#define Trace printf

#ifdef TRACE_LSEG
#define dbg_lseg printf
#else
#define dbg_lseg(x...) do { } while (0)
#endif

#if TRACE
#define dbg Trace
#else
#define dbg
#endif

#if defined(MOUNTER_LOG)
// Host-provided log sink (e.g. the Poseidon debug backend). It may format with
// exec RawDoFmt, so format strings here use %l-sized conversions only (no %p).
void mounter_log(const char *fmt, ...);
#define printf mounter_log
#else
#define printf(...)
#endif
#define MAX_BLOCKSIZE 4096
#define LSEG_DATASIZE (512 / 4 - 5)

#if NO_CONFIGDEV
extern UBYTE entrypoint, entrypoint_end;
extern UBYTE bootblock, bootblock_end;
#endif

struct MountData
{
	struct ExecBase *SysBase;
	struct ExpansionBase *ExpansionBase;
	struct DosLibrary *DOSBase;
	struct IOExtTD *request;
	struct ConfigDev *configDev;
	const UBYTE *creator;
	const UBYTE *devicename;

	ULONG lsegblock;
	ULONG lseglongs;
	ULONG lsegoffset;
	struct LoadSegBlock *lsegbuf;
	UWORD lsegwordbuf;
	UWORD lseghasword;

	ULONG unitnum;
	UBYTE zero[2];
	BOOL wasLastDev;
	BOOL wasLastLun;
	BOOL slowSpinup;
	int blocksize;
	ULONG totalsectors;

	ULONG flags;                    /* MSF_* */
	const struct MountFS *fatFS;
	const struct MountFS *ntfsFS;
	const struct MountFS *cdFS;
	BOOL legacyMounted;             /* per unit, for MSF_LEGACY_FIRST_ONLY */
};

// Classic recipe: FAT95 dostype, FileSystem.resource only.
static const struct MountFS defaultFatFS = { 0x46415401, NULL, NULL, NULL, 0, 0, 0 };

// Copy a C string into a freshly allocated BCPL string: length byte, chars, and
// a trailing NUL (from MEMF_CLEAR) so it doubles as a C string. Used for
// dn_Handler / de_Control, which the DeviceNode owns for its lifetime — so the
// allocation is deliberately never freed.
static UBYTE *AllocBSTR(const UBYTE *s, struct ExecBase *SysBase)
{
	int len = strlen((const char *)s);
	if (len > 255)      // a BSTR length prefix is a single byte
		len = 255;
	UBYTE *b = AllocMem(len + 2, MEMF_PUBLIC | MEMF_CLEAR);
	if (b) {
		b[0] = len;
		memcpy(b + 1, s, len);
	}
	return b;
}

// Buffer for a mount's DOS device name, sized like the RDB pb_DriveName[32]
// field it mirrors: a 32-byte BSTR holds a 31-char name (the RDB limit), and our
// NUL-terminated convention (length byte + chars + NUL) caps usable names at 30.
#define DEVNAME_BUFSIZE 32

// Seed a DOS device-name BSTR (length byte, chars, trailing NUL) into the
// caller's cap-byte buffer. A trailing digit is ensured ("UMSD" -> "UMSD0") so
// units number from 0; a name already ending in a digit is left as given.
// CheckAndFixDevName() appends any collision digits later, in the same buffer.
static void MakeDevName(UBYTE *dst, const UBYTE *s, int cap)
{
	// cap-2 chars fit the buffer (length byte + NUL); reserve one more for the
	// unit digit we may append below.
	int len = strlen((const char *)s);
	if (len > cap - 3)
		len = cap - 3;
	memcpy(dst + 1, s, len);
	if (len == 0 || dst[len] < '0' || dst[len] > '9') {
		dst[len + 1] = '0';
		len++;
	}
	dst[0] = len;
	dst[len + 1] = 0;
}

#define SCSI_CD_MAX_TRACKS 100
#define SCSI_CMD_READ_TOC 0x43

struct __packed SCSI_TOC_TRACK_DESCRIPTOR {
    UBYTE reserved1;
    UBYTE adrControl;
    UBYTE trackNumber;
    UBYTE reserved2;
    UBYTE reserved3;
    UBYTE minute;
    UBYTE second;
    UBYTE frame;
};

struct __packed __attribute__((aligned(2))) SCSI_CD_TOC {
    UWORD length;
    UBYTE firstTrack;
    UBYTE lastTrack;
    struct SCSI_TOC_TRACK_DESCRIPTOR td[SCSI_CD_MAX_TRACKS];
};

// Get Block size of unit
BYTE GetGeometry(struct IOExtTD *req, struct DriveGeometry *geometry)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
	struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop

	req->iotd_Req.io_Command = TD_GETGEOMETRY;
	req->iotd_Req.io_Data    = geometry;
	req->iotd_Req.io_Length  = sizeof(struct DriveGeometry);

	return DoIO((struct IORequest *)req);
}

static void W_NewList(struct List *new_list)
{
    new_list->lh_Head = (struct Node *)&new_list->lh_Tail;
    new_list->lh_Tail = 0;
    new_list->lh_TailPred = (struct Node *)new_list;
}

// KS 1.3 compatibility functions
APTR W_CreateIORequest(struct MsgPort *ioReplyPort, ULONG size, struct ExecBase *SysBase)
{
	struct IORequest *ret = NULL;
	if(ioReplyPort == NULL)
		return NULL;
	ret = (struct IORequest*)AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
	if(ret != NULL)
	{
		ret->io_Message.mn_ReplyPort = ioReplyPort;
		ret->io_Message.mn_Length = size;
	}
	return ret;
}
void W_DeleteIORequest(APTR iorequest, struct ExecBase *SysBase)
{
	if(iorequest != NULL) {
		FreeMem(iorequest, ((struct Message*)iorequest)->mn_Length);
	}
}
struct MsgPort *W_CreateMsgPort(struct ExecBase *SysBase)
{
	struct MsgPort *ret;
	ret = (struct MsgPort*)AllocMem(sizeof(struct MsgPort), MEMF_PUBLIC | MEMF_CLEAR);
	if(ret != NULL)
	{
		BYTE sb = AllocSignal(-1);
		if (sb != -1)
		{
			ret->mp_Flags = PA_SIGNAL;
			ret->mp_Node.ln_Type = NT_MSGPORT;
			W_NewList(&ret->mp_MsgList);
			ret->mp_SigBit = sb;
			ret->mp_SigTask = FindTask(NULL);
			return ret;
		}
		FreeMem(ret, sizeof(struct MsgPort));
	}
	return NULL;
}
void W_DeleteMsgPort(struct MsgPort *port, struct ExecBase *SysBase)
{
	if(port != NULL)
	{
		FreeSignal(port->mp_SigBit);
		FreeMem(port, sizeof(struct MsgPort));
	}
}

// Flush cache (Filesystem relocation)
static void cacheclear(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	if (SysBase->LibNode.lib_Version >= 37) {
		CacheClearU();
	}
}

// Simply memory copy.
// Only used for few short copies, it does not need to be optimal.
// Required because compiler built-in memcpy() can have
// extra dependencies which will make boot rom build
// impossible.
static void copymem(void *dstp, void *srcp, UWORD size)
{
	UBYTE *dst = (UBYTE*)dstp;
	UBYTE *src = (UBYTE*)srcp;
	while (size != 0) {
		*dst++ = *src++;
		size--;
	}
}

// Check block checksum
static UWORD checksum(UBYTE *buf, struct MountData *md)
{
	ULONG chk = 0;
	ULONG num_longs;

	num_longs = (buf[4] << 24) | (buf[5] << 16) | (buf[6] << 8) | (buf[7]);
	// A block's summed-longs count can never exceed the block itself; reject
	// garbage counts so the checksum loop can't read past the sector buffer.
	if (num_longs > (ULONG)md->blocksize / sizeof(LONG))
		return FALSE;

	for (UWORD i = 0; i < (int)(num_longs * sizeof(LONG)); i += 4) {
		ULONG v = (buf[i + 0] << 24) | (buf[i + 1] << 16) | (buf[i + 2] << 8) | (buf[i + 3 ] << 0);
		chk += v;
	}
	if (chk) {
		dbg("Checksum error %08"PRIx32"\n", chk);
		return FALSE;
	}
	return TRUE;
}


#define MAX_RETRIES 3

// Read single block with retries
static BOOL readblock(UBYTE *buf, ULONG block, ULONG id, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct IOExtTD *request = md->request;
	UWORD i, max_retries = MAX_RETRIES;
	if (md->slowSpinup)
		max_retries = 15;

	// Byte offsets past 4GB need TD_READ64 (io_Actual = high 32 bits)
	uint64_t offset = (uint64_t)block * (ULONG)md->blocksize;
	request->iotd_Req.io_Command = (offset >> 32) ? TD_READ64 : CMD_READ;
	request->iotd_Req.io_Actual = offset >> 32;
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
	dbg_lseg("Read block %"PRIu32" %08"PRIx32"\n", block, v);
	if (id != 0xffffffff) {
		if (v != id) {
			return FALSE;
		}
		if (!checksum(buf, md)) {
			return FALSE;
		}
	}
	return TRUE;
}

// Read multiple longs from LSEG blocks
static BOOL lseg_read_longs(struct MountData *md, ULONG longs, ULONG *data)
{
	dbg_lseg("lseg_read_longs, longs %"PRId32"  ptr 0x%08lx, remaining %"PRId32"\n", longs, (ULONG)data, md->lseglongs);
	ULONG cnt = 0;
	md->lseghasword = FALSE;
	while (longs > cnt) {
		if (md->lseglongs > 0) {
			data[cnt] = md->lsegbuf->lsb_LoadData[md->lsegoffset];
			md->lsegoffset++;
			md->lseglongs--;
			cnt++;
			if (longs == cnt) {
				return TRUE;
			}
		}
		if (!md->lseglongs) {
			if (md->lsegblock == 0xffffffff) {
				dbg("lseg_read_long premature end!\n");
				return FALSE;
			}
			if (!readblock((UBYTE*)md->lsegbuf, md->lsegblock, IDNAME_LOADSEG, md)) {
				return FALSE;
			}
			md->lseglongs = LSEG_DATASIZE;
			md->lsegoffset = 0;
			dbg_lseg("lseg_read_long lseg block %"PRId32" loaded, next %"PRId32"\n", md->lsegblock, md->lsegbuf->lsb_Next);
			md->lsegblock = md->lsegbuf->lsb_Next;
		}
	}
	return TRUE;
}
// Read single long from LSEG blocks
static BOOL lseg_read_long(struct MountData *md, ULONG *data)
{
	BOOL v;
	if (md->lseghasword) {
		ULONG temp;
		v = lseg_read_longs(md, 1, &temp);
		*data = (md->lsegwordbuf << 16) | (temp >> 16);
		md->lsegwordbuf = (UWORD)temp;
		md->lseghasword = TRUE;
	} else {
		v = lseg_read_longs(md, 1, data);
	}
	dbg_lseg("lseg_read_long %08"PRIx32"\n", *data);
	return v;
}
// Read single word from LSEG blocks
// Internally reads long and buffers second word.
static BOOL lseg_read_word(struct MountData *md, ULONG *data)
{
	if (md->lseghasword) {
		*data = md->lsegwordbuf;
		md->lseghasword = FALSE;
		dbg("lseg_read_word 2/2 %08"PRIx32"\n", *data);
		return TRUE;
	}
	ULONG temp;
	BOOL v = lseg_read_longs(md, 1, &temp);
	if (v) {
		md->lseghasword = TRUE;
		md->lsegwordbuf = (UWORD)temp;
		*data = temp >> 16;
	}
	dbg("lseg_read_word 1/2 %08"PRIx32"\n", *data);
	return v;
}

struct RelocHunk
{
	ULONG hunkSize;
	ULONG *hunkData;
};

// Filesystem relocator
static APTR fsrelocate(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	ULONG data;
	struct RelocHunk *relocHunks;
	LONG firstHunk, lastHunk;
	ULONG totalHunks;
	UWORD hunkCnt;
	WORD ret = 0;
	APTR firstProcessedHunk = NULL;

	if (!lseg_read_long(md, &data)) {
		return NULL;
	}
	if (data != HUNK_HEADER) {
		return NULL;
	}
	// Read the size of a resident library name. This should
	// never be != 0.
	if (!lseg_read_long(md, &data) || data != 0) {
		return NULL;
	}
	// Read the size of the hunk table, which should be > 0.
	// Note that this number may be larger than the
	// difference between the last and the first hunk + 1 for
	// overlay binary files. But then this function does not
	// support overlay binary files.
	if (!lseg_read_long(md, &data) || data <= 0) {
		return NULL;
	}
	// first hunk
	if (!lseg_read_long(md, &firstHunk)) {
		return NULL;
	}
	// last hunk
	if (!lseg_read_long(md, &lastHunk)) {
		return NULL;
	}
	if (firstHunk < 0 || lastHunk < 0 || firstHunk > lastHunk) {
		return NULL;
	}
	totalHunks = lastHunk - firstHunk + 1;
	dbg("first hunk %"PRId32", last hunk %"PRId32"\n", firstHunk, lastHunk);
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
		if (!lseg_read_long(md, &hunkHeadSize)) {
			goto end;
		}
		if ((hunkHeadSize & (HUNKF_CHIP | HUNKF_FAST)) == (HUNKF_CHIP | HUNKF_FAST)) {
			if (!lseg_read_long(md, &memoryFlags)) {
				goto end;
			}
		} else if (hunkHeadSize & HUNKF_CHIP) {
			memoryFlags |= MEMF_CHIP;
		}
		hunkHeadSize &= ~(HUNKF_CHIP | HUNKF_FAST);
		rh->hunkSize = hunkHeadSize;
		rh->hunkData = AllocMem((hunkHeadSize + 2) * sizeof(ULONG), memoryFlags | MEMF_CLEAR);
		if (!rh->hunkData) {
			goto end;
		}
		dbg("hunk %"PRId32": ptr 0x%08lx, size %"PRId32", memory flags %08"PRIx32"\n", hunkCnt + firstHunk, (ULONG)rh->hunkData, hunkHeadSize, memoryFlags);
		rh->hunkData[0] = rh->hunkSize + 2;
		rh->hunkData[1] = MKBADDR(prevChunk);
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
		if (!lseg_read_long(md, &hunkType)) {
			if (hunkCnt >= totalHunks) {
				break;  // normal end
			}
			goto end;
		}
		dbg("HUNK %08"PRIx32"\n", hunkType);
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
				if (!lseg_read_long(md, &hunkSize)) {
					goto end;
				}
				if (hunkSize > rh->hunkSize) {
					goto end;
				}
				if (hunkType != HUNK_BSS) {
					if (!lseg_read_longs(md, hunkSize, rh->hunkData)) {
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
					if (!lseg_read_long(md, &relocCnt)) {
						goto end;
					}
					if (!relocCnt) {
						break;
					}
					if (!lseg_read_long(md, &relocHunk)) {
						goto end;
					}
					relocHunk -= firstHunk;
					if (relocHunk >= totalHunks) {
						goto end;
					}
					dbg("HUNK_RELOC32: relocs %"PRId32" hunk %"PRId32"\n", relocCnt, relocHunk + firstHunk);
					struct RelocHunk *rhr = &relocHunks[relocHunk];
					while (relocCnt != 0) {
						ULONG relocOffset;
						if (hunkType == HUNK_RELOC32SHORT) {
							if (!lseg_read_word(md, &relocOffset)) {
								goto end;
							}
						} else {
							if (!lseg_read_long(md, &relocOffset)) {
								goto end;
							}
						}
						if (relocOffset > (rh->hunkSize - 1) * sizeof(ULONG)) {
							goto end;
						}
						UBYTE *hData = (UBYTE*)rh->hunkData + relocOffset;
						if (relocOffset & 1) {
							// Odd address, 68000/010 support.
							ULONG v = (hData[0] << 24) | (hData[1] << 16) | (hData[2] << 8) | (hData[3] << 0);
							v += (ULONG)rhr->hunkData;
							hData[0] = v >> 24;
							hData[1] = v >> 16;
							hData[2] = v >>  8;
							hData[3] = v >>  0;
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
			struct RelocHunk *rh = &relocHunks[hunkCnt];
			if (rh->hunkData) {
				FreeMem(rh->hunkData - 2, (rh->hunkSize + 2) * sizeof(ULONG));
			}
			hunkCnt++;
		}
		firstProcessedHunk = NULL;
	} else {
		cacheclear(md);
		dbg("reloc ok, first hunk 0x%08lx\n", (ULONG)firstProcessedHunk);
	}

	FreeMem(relocHunks, totalHunks * sizeof(struct RelocHunk));

	return firstProcessedHunk;
}

// Scan FileSystem.resource, create new if it is not found or existing entry has older version number.
static struct FileSysEntry *FSHDProcess(struct FileSysHeaderBlock *fshb, ULONG dostype, ULONG version, BOOL newOnly, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct FileSysEntry *result_fse = NULL;
	const UBYTE *creator = md->creator ? md->creator : md->zero;
	const char resourceName[] = "FileSystem.resource";

	Forbid();
	struct FileSysResource *fsr = OpenResource(FSRNAME);
	if (!fsr) {
		// FileSystem.resource didn't exist (KS 1.3), create it.
		fsr = AllocMem(sizeof(struct FileSysResource) + strlen(resourceName) + 1 + strlen((const char *)creator) + 1, MEMF_PUBLIC | MEMF_CLEAR);
		if (fsr) {
			char *FsResName  = (char *)(fsr + 1);
			char *CreatorStr = (char *)FsResName + (strlen(resourceName) + 1);
			W_NewList(&fsr->fsr_FileSysEntries);
			fsr->fsr_Node.ln_Type = NT_RESOURCE;
			strcpy(FsResName, resourceName);
			fsr->fsr_Node.ln_Name = FsResName;
			strcpy(CreatorStr, (const char *)creator);
			fsr->fsr_Creator = CreatorStr;
			AddTail(&SysBase->ResourceList, &fsr->fsr_Node);
		}
		dbg("FileSystem.resource created 0x%08lx\n", (ULONG)fsr);
	}

	if (fsr) {
		struct Node *node;
		struct FileSysEntry *found_existing_fse = NULL;

		// Correctly iterate through the list to find if an entry for 'dostype' already exists
		for (node = fsr->fsr_FileSysEntries.lh_Head;
			 node->ln_Succ != NULL; // Standard AmigaOS list traversal: loop while node is not the tail sentinel
			 node = node->ln_Succ) {
			struct FileSysEntry *current_entry = (struct FileSysEntry *)node;
			if (current_entry->fse_DosType == dostype) {
				found_existing_fse = current_entry; // Found a match by DosType
				break; // Process this first match
			}
		}

		if (found_existing_fse) {
			// An entry with the same DosType was found
			if (found_existing_fse->fse_Version >= version) {
				if (newOnly) {
					// Existing entry is suitable, and we only want to add a new one if necessary.
					dbg("FileSystem.resource scan: Existing up-to-date entry 0x%08lx for 0x%08lx found. Version 0x%08lx >= requested 0x%08lx. No action needed.\n",
						(ULONG)found_existing_fse, dostype, found_existing_fse->fse_Version, version);
					Permit();
					return NULL; // Indicate no new/updated fse needed from this call
				} else {
					// newOnly is false. We found an existing entry.
					result_fse = found_existing_fse;
				}
			}
		}
		// If found_existing_fse is NULL, no entry for this dostype was found.

		// If fshb is provided (i.e., we have a FileSystem definition from RDB/disk)
		// AND newOnly is true (caller wants to add this if it's new or an upgrade)
		// AND we haven't already decided to return an existing (up-to-date, !newOnly) entry:
		if (fshb && newOnly) {
			if (!(found_existing_fse && found_existing_fse->fse_Version >= version)) {
				// Either no existing FSE for this DosType, or existing one is older.
				// So, we create a new one based on fshb.
				result_fse = AllocMem(sizeof(struct FileSysEntry) + strlen((const char *)creator) + 1, MEMF_PUBLIC | MEMF_CLEAR);
				if (result_fse) {
					ULONG patchFlags = fshb->fhb_PatchFlags;
					if (patchFlags & 0x0001)
						result_fse->fse_Type = fshb->fhb_Type;
					if (patchFlags & 0x0002)
						result_fse->fse_Task = fshb->fhb_Task;
					if (patchFlags & 0x0004)
						result_fse->fse_Lock = fshb->fhb_Lock;
					if (patchFlags & 0x0008)
						result_fse->fse_Handler = fshb->fhb_Handler;
					if (patchFlags & 0x0010)
						result_fse->fse_StackSize = fshb->fhb_StackSize;
					if (patchFlags & 0x0020)
						result_fse->fse_Priority = fshb->fhb_Priority;
					if (patchFlags & 0x0040)
						result_fse->fse_Startup = fshb->fhb_Startup;
					if (patchFlags & 0x0080)
						result_fse->fse_SegList = fshb->fhb_SegListBlocks;
					if (patchFlags & 0x0100)
						result_fse->fse_GlobalVec = fshb->fhb_GlobalVec;
					result_fse->fse_DosType = fshb->fhb_DosType;
					result_fse->fse_Version = fshb->fhb_Version;
					result_fse->fse_PatchFlags = fshb->fhb_PatchFlags;
					strcpy((char *)(result_fse + 1), (const char *)creator);
					result_fse->fse_Node.ln_Name = (UBYTE *)(result_fse + 1);
					dbg("FileSystem.resource scan: new FileSysEntry 0x%08lx created for 0x%08lx based on fshb.\n", (ULONG)result_fse, dostype);
				}
			}
		} else if (fshb && !newOnly && found_existing_fse) {
			result_fse = found_existing_fse;
		}
	}
	Permit();
	return result_fse;
}

// Add new FileSysEntry to FileSystem.resource or free it if filesystem load failed.
static void FSHDAdd(struct FileSysEntry *fse, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	if (fse->fse_SegList) {
		Forbid();
		struct FileSysResource *fsr = OpenResource(FSRNAME);
		if (fsr) {
			AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
			dbg("FileSysEntry 0x%08lx added to FileSystem.resource, dostype %08"PRIx32"\n", (ULONG)fse, fse->fse_DosType);
			fse = NULL;
		}
		Permit();
	}
	if (fse) {
		dbg("FileSysEntry 0x%08lx freed, dostype %08"PRIx32"\n", (ULONG)fse, fse->fse_DosType);
		FreeMem(fse, sizeof(struct FileSysEntry));
	}
}

// Parse FileSystem Header Blocks, load and relocate filesystem if needed.
static struct FileSysEntry *ParseFSHD(ULONG block, ULONG dostype, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct FileSysEntry *fse = NULL;
	// The FileSysHeaderBlock and its LoadSegBlock chain live in two scratch
	// sectors that must coexist; both are freed before returning.
	UBYTE *buf = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);
	UBYTE *segbuf = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);

	if (buf && segbuf) {
		struct FileSysHeaderBlock *fshb = (struct FileSysHeaderBlock*)buf;
		for (;;) {
			if (block == 0xffffffff) {
				break;
			}
			if (!readblock(buf, block, IDNAME_FILESYSHEADER, md)) {
				break;
			}
			dbg("FSHD found, block %"PRIu32", dostype %08"PRIx32", looking for dostype %08"PRIx32"\n", block, fshb->fhb_DosType, dostype);
			if (fshb->fhb_DosType == dostype) {
				dbg("FSHD dostype match found\n");
				fse = FSHDProcess(fshb, dostype, fshb->fhb_Version, TRUE, md);
				if (fse) {
					md->lsegblock = fshb->fhb_SegListBlocks;
					md->lsegbuf = (struct LoadSegBlock*)segbuf;
					md->lseglongs = 0;
					APTR seg = fsrelocate(md);
					fse->fse_SegList = MKBADDR(seg);
					// Add to FileSystem.resource if succeeded, delete entry if failure.
					FSHDAdd(fse, md);
				}
				break;
			}
			block = fshb->fhb_Next;
		}
	}
	if (!fse) {
		fse = FSHDProcess(NULL, dostype, 0, FALSE, md);
	}
	if (segbuf) FreeMem(segbuf, MAX_BLOCKSIZE);
	if (buf)    FreeMem(buf, MAX_BLOCKSIZE);
	return fse;
}

#if NO_CONFIGDEV
// Create fake ConfigDev and DiagArea to support autoboot without requiring real autoconfig device.
static void CreateFakeConfigDev(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	struct ConfigDev *configDev;

	configDev = AllocConfigDev();
	if (configDev) {
		configDev->cd_BoardAddr = (void*)&entrypoint;
		configDev->cd_BoardSize = (UBYTE*)&entrypoint_end - (UBYTE*)&entrypoint;
		configDev->cd_Rom.er_Type = ERTF_DIAGVALID;
		ULONG bbSize = &bootblock_end - &bootblock;
		ULONG daSize = sizeof(struct DiagArea) + bbSize;
		struct DiagArea *diagArea = AllocMem(daSize, MEMF_CLEAR | MEMF_PUBLIC);
		if (diagArea) {
			diagArea->da_Config = DAC_CONFIGTIME;
			diagArea->da_BootPoint = sizeof(struct DiagArea);
			diagArea->da_Size = (UWORD)daSize;
			copymem(diagArea + 1, &bootblock, bbSize);
			memcpy(&configDev->cd_Rom.er_Reserved0c, &diagArea, sizeof(ULONG));
			cacheclear(md);
		}
		md->configDev = configDev;
	}
}
#endif

struct ParameterPacket
{
	const UBYTE *dosname;
	const UBYTE *execname;
	ULONG unitnum;
	ULONG flags;
	struct DosEnvec de;
};

static UBYTE ToUpper(UBYTE c)
{
	if (c >= 'a' && c <= 'z') {
		return c - ('a'-'A');
	}
	return c;
}

// Case-insensitive BSTR string comparison
static BOOL CompareBSTRNoCase(const UBYTE *src1, const UBYTE *src2)
{
	UBYTE len1 = *src1++;
	UBYTE len2 = *src2++;
	if (len1 != len2) {
		return FALSE;
	}
	for (UWORD i = 0; i < len1; i++) {
		UBYTE c1 = *src1++;
		UBYTE c2 = *src2++;
		c1 = ToUpper(c1);
		c2 = ToUpper(c2);
		if (c1 != c2) {
			return FALSE;
		}
	}
	return TRUE;
}

// Check for duplicate device names
static bool CheckDevName(struct MountData *md, UBYTE *bname)
{
	struct ExecBase *SysBase = md->SysBase;
	bool found = false;

	Forbid();
	struct BootNode *bn;
	for (bn = (struct BootNode*)md->ExpansionBase->MountList.lh_Head;
		 bn->bn_Node.ln_Succ != NULL;
		 bn = (struct BootNode*)bn->bn_Node.ln_Succ)
	{
		struct DeviceNode *dn = bn->bn_DeviceNode;
		const UBYTE *bname2 = BADDR(dn->dn_Name);
		if (CompareBSTRNoCase(bname, bname2)) {
			found = true;
		}
	}

	Permit();

	// Post-boot mounts go straight to the DOS lists, not eb_MountList — check
	// those too (devices, volumes and assigns all claim the name).
	if (!found && md->DOSBase && md->DOSBase->dl_lib.lib_Version >= 36) {
		struct DosLibrary *DOSBase = md->DOSBase;
		struct DosList *dl = LockDosList(LDF_ALL | LDF_READ);
		if (FindDosEntry(dl, (STRPTR)(bname + 1), LDF_ALL)) {
			found = true;
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
static void CheckAndFixDevName(struct MountData *md, UBYTE *bname, int cap)
{
	int maxlen = cap - 2;
	while (CheckDevName(md, bname)) {
		UBYTE len = bname[0] > maxlen ? maxlen : bname[0];
		UBYTE *name = bname + 1;
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

// Add DeviceNode to Expansion MountList.
static void AddNode(struct PartitionBlock *part, struct ParameterPacket *pp, struct DeviceNode *dn, UBYTE *name, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	struct DosLibrary *DOSBase = md->DOSBase;

	LONG bootPri = (part->pb_Flags & PBFF_BOOTABLE) ? pp->de.de_BootPri : -128;
	if (md->flags & MSF_NO_BOOT)
		bootPri = -128;
	if (ExpansionBase->LibNode.lib_Version >= 37) {
		// KS 2.0+
		if (!md->DOSBase && bootPri > -128) {
			dbg("KS20+ Mounting as bootable: pri %08"PRIx32"\n", bootPri);
			AddBootNode(bootPri, ADNF_STARTPROC, dn, md->configDev);
		} else {
			dbg("KS20+: Mounting as non-bootable\n");
			AddDosNode(bootPri, ADNF_STARTPROC, dn);
		}
	} else {
		// KS 1.3
		if (!md->DOSBase && bootPri > -128) {
			dbg("KS13 Mounting as bootable: pri %08"PRIx32"\n", bootPri);
			// Create and insert bootnode manually.
			struct BootNode *bn = AllocMem(sizeof(struct BootNode), MEMF_CLEAR | MEMF_PUBLIC);
			if (bn) {
				bn->bn_Node.ln_Type = NT_BOOTNODE;
				bn->bn_Node.ln_Pri = (BYTE)bootPri;
				bn->bn_Node.ln_Name = (UBYTE*)md->configDev;
				bn->bn_DeviceNode = dn;
				Forbid();
				Enqueue(&md->ExpansionBase->MountList, &bn->bn_Node);
				Permit();
			}
		} else {
			dbg("KS13: Mounting as non-bootable\n");
			AddDosNode(bootPri, 0, dn);
			if (md->DOSBase) {
				// KS 1.3 ADNF_STARTPROC is not supported; start the filesystem
				// process via DeviceProc(). Build "<name>:" in a local buffer —
				// appending ':' into 'name' (a pointer into the 32-byte
				// pb_DriveName) would overflow the field for a full-length name.
				UBYTE devpath[36];
				UWORD len = strlen(name);
				if (len > (int)sizeof(devpath) - 2)
					len = (int)sizeof(devpath) - 2;
				memcpy(devpath, name, len);
				devpath[len++] = ':';
				devpath[len] = 0;
				void * __attribute__((unused)) mp = DeviceProc(devpath);
				dbg("DeviceProc() returned 0x%08lx\n", (ULONG)mp);
			}
		}
	}
}

static void ProcessPatchFlags(struct DeviceNode *dn, struct FileSysEntry *fse)
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

static struct FileSysEntry *find_filesystem(ULONG id1, ULONG id2, struct ExecBase *SysBase);

// TRUE if the recipe can resolve to a working filesystem: the dostype is
// registered in FileSystem.resource or a handler file is given. Checked
// before creating a DeviceNode so unresolvable partitions are skipped clean.
static BOOL FileSystemAvailable(struct MountData *md, const struct MountFS *fs)
{
	return find_filesystem(fs->dosType, 0, md->SysBase) != NULL || fs->handler != NULL;
}

// Attach handler information to a fresh DeviceNode: the FileSystem.resource
// entry if the dostype is registered, else the recipe's handler file (loaded
// by DOS on first access).
static BOOL SetupFileSystem(struct MountData *md, struct DeviceNode *dn, const struct MountFS *fs)
{
	struct ExecBase *SysBase = md->SysBase;
	struct FileSysEntry *fse = find_filesystem(fs->dosType, 0, SysBase);
	if (fse) {
		ProcessPatchFlags(dn, fse);
		return TRUE;
	}
	if (fs->handler) {
		UBYTE *hb = AllocBSTR(fs->handler, SysBase);
		if (hb) {
			dn->dn_Handler = MKBADDR(hb);
			dn->dn_GlobalVec = (BPTR)-1;   /* C handler convention */
			dn->dn_StackSize = fs->stackSize ? fs->stackSize : 8192;
			return TRUE;
		}
	}
	return FALSE;
}

// Add a legacy/CD DeviceNode: bootable only pre-DOS, same rule as AddNode().
static void AddLegacyNode(struct MountData *md, LONG bootPri, struct DeviceNode *dn)
{
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	if (!md->DOSBase && bootPri > -128 && !(md->flags & MSF_NO_BOOT)) {
		AddBootNode(bootPri, ADNF_STARTPROC, dn, md->configDev);
	} else {
		AddDosNode(bootPri, ADNF_STARTPROC, dn);
	}
}

// Parse PART block, mount drive. Returns the next PART block in the chain;
// increments *mounted on success (the return slot is taken by the chain link).
static ULONG ParsePART(UBYTE *buf, ULONG block, ULONG filesysblock, struct MountData *md, LONG *mounted)
{
	struct ExecBase *SysBase = md->SysBase;
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;
	struct PartitionBlock *part = (struct PartitionBlock*)buf;
	ULONG nextpartblock = 0xffffffff;

	if (!readblock(buf, block, IDNAME_PARTITION, md)) {
		return nextpartblock;
	}
	dbg("PART found, block %"PRIu32"\n", block);
	nextpartblock = part->pb_Next;
	if (!(part->pb_Flags & PBFF_NOMOUNT)) {
		struct ParameterPacket *pp = AllocMem(sizeof(struct ParameterPacket), MEMF_PUBLIC | MEMF_CLEAR);
		if (pp) {
			UBYTE len;
			copymem(&pp->de, &part->pb_Environment, (part->pb_Environment[0] + 1) * sizeof(ULONG));
			struct FileSysEntry *fse = ParseFSHD(filesysblock, pp->de.de_DosType, md);
			pp->execname = md->devicename;
			pp->unitnum = md->unitnum;
			pp->dosname = part->pb_DriveName + 1;
			// The RDB length byte is untrusted disk data. Clamp both it and the
			// content to what the field holds (length byte + chars + NUL) so every
			// downstream BSTR/C-string view of the name agrees on the length.
			len = *part->pb_DriveName;
			if (len > (int)sizeof(part->pb_DriveName) - 2)
				len = (int)sizeof(part->pb_DriveName) - 2;
			part->pb_DriveName[0] = len;
			part->pb_DriveName[len + 1] = 0;
			dbg("PART '%s'\n", pp->dosname);
			CheckAndFixDevName(md, part->pb_DriveName, sizeof(part->pb_DriveName));
			struct DeviceNode *dn = MakeDosNode(pp);
			if (dn) {
				if (fse) {
					ProcessPatchFlags(dn, fse);
				}
				dbg("Mounting partition\n");
#if NO_CONFIGDEV
				if (!md->configDev && !md->DOSBase) {
					CreateFakeConfigDev(md);
				}
#endif
				AddNode(part, pp, dn, part->pb_DriveName + 1, md);
				(*mounted)++;
			} else {
				dbg("Device node creation failed\n");
			}
			FreeMem(pp, sizeof(struct ParameterPacket));
		}
	}
	return nextpartblock;
}

// Scan PART blocks. Returns the number of partitions mounted from this disk.
static LONG ParseRDSK(UBYTE *buf, struct MountData *md)
{
	struct RigidDiskBlock *rdb = (struct RigidDiskBlock*)buf;
	ULONG partblock = rdb->rdb_PartitionList;
	ULONG filesysblock = rdb->rdb_FileSysHeaderList;
	ULONG flags = rdb->rdb_Flags;
	LONG mounted = 0;
	for (;;) {
		if (partblock == 0xffffffff) {
			break;
		}
		partblock = ParsePART(buf, partblock, filesysblock, md, &mounted);
	}

	md->wasLastDev = (flags & RDBFF_LAST) != 0;
	md->wasLastLun = (flags & RDBFF_LASTLUN) != 0;

	return mounted;
}

// Search for RDB
static LONG ScanRDSK(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	UBYTE *buf = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);
	if (!buf)
		return -1;
	LONG ret = -1;
	for (UWORD i = 0; i < RDB_LOCATION_LIMIT; i++) {
		if (readblock(buf, i, 0xffffffff, md)) {
			struct RigidDiskBlock *rdb = (struct RigidDiskBlock*)buf;
			if (rdb->rdb_ID == IDNAME_RIGIDDISK) {
				dbg("RDB found, block %"PRIu32"\n", i);
				ret = ParseRDSK(buf, md);
				break;
			}
		}
	}
	FreeMem(buf, MAX_BLOCKSIZE);
	return ret;
}

static struct FileSysEntry *find_filesystem(ULONG id1, ULONG id2, struct ExecBase *SysBase)
{
	struct FileSysResource *FileSysResBase = NULL;
	struct FileSysEntry *fse, *fs=NULL;
	Forbid();
	if ((FileSysResBase = (struct FileSysResource *)OpenResource(FSRNAME))) {
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

// Check if there is a disc inserted
static bool UnitIsReady(struct IOStdReq *req)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
	struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop

	BYTE err;

	// First spin up the disc
	// Not critical if there's an error so no need to check
	req->io_Command = CMD_START;
	req->io_Error   = 0;
	DoIO((struct IORequest *)req);

	req->io_Command = TD_CHANGESTATE;
	req->io_Actual  = 0;
	req->io_Error   = 0;
	err = DoIO((struct IORequest *)req);

	// Some devices/units don't support this - assume that it is ready
	if (err == IOERR_NOCMD) return true;

	if (err == 0 && req->io_Actual == 0) return true;

	return false;
}


// Check if this is a data disc by reading the TOC and checking that track 1 is a data track.
static bool isDataCD(struct IOStdReq *ior)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds="
	struct ExecBase *SysBase = *(struct ExecBase **)4UL;
#pragma GCC diagnostic pop
	bool ret = false;

	BYTE err;

	struct SCSICmd     *scsiCmd = NULL;
	struct SCSI_CD_TOC *tocBuf  = NULL;

	ULONG bufSize = sizeof(struct SCSI_CD_TOC);

	char cdb[10];
	memset(&cdb,0,10);

	if ((scsiCmd = AllocMem(sizeof(struct SCSICmd),MEMF_PUBLIC | MEMF_CLEAR))) {
		if ((tocBuf = AllocMem(bufSize,MEMF_PUBLIC | MEMF_CLEAR))) {
			scsiCmd->scsi_Data      = (UWORD *)tocBuf;
			scsiCmd->scsi_Length    = bufSize;
			scsiCmd->scsi_Flags     = SCSIF_READ;
			scsiCmd->scsi_CmdLength = 10;
			scsiCmd->scsi_Command   = cdb;

			cdb[0] = SCSI_CMD_READ_TOC;
			cdb[2] = 0;                  // Format: 0
			cdb[6] = 1;                  // Track 1
			cdb[7] = bufSize >> 8;
			cdb[8] = bufSize & 0xFF;

			ior->io_Data    = scsiCmd;
			ior->io_Length  = sizeof(struct SCSICmd);
			ior->io_Command = HD_SCSICMD;

			for (int retry = 0; retry < 3; retry++) {
				if ((err = DoIO((struct IORequest *)ior)) == 0 && scsiCmd->scsi_Status == 0)
					break;
			}

			if (err == 0) {
				if (tocBuf->firstTrack == 1 && tocBuf->td[0].trackNumber == 1) {
					if (tocBuf->td[0].adrControl & 0x04) {	// Data Track?
						ret = true;
					}
				}
			}

			FreeMem(tocBuf,bufSize);
		}
		FreeMem(scsiCmd,sizeof(struct SCSICmd));
	}
	return ret;
}

// CheckPVD
// Check for "CDTV" or "AMIGA BOOT" as the System ID in the PVD
// Returns: -1 on error, 0 if not CDTV/AMIGA BOOT, 1 if bootable
static LONG CheckPVD(struct IOStdReq *ior, struct ExecBase *SysBase)
{
	const char sys_id_1[] = "CDTV";
	const char sys_id_2[] = "AMIGA BOOT";
	const char iso_id[]   = "CD001";

	BYTE err = 0;
	LONG ret = -1;
	char *buf = NULL;

	if (!(buf = AllocMem(2048,MEMF_ANY|MEMF_CLEAR))) goto done;

	char *id_string = buf + 1;
	char *system_id = buf + 8;

	ior->io_Command = CMD_READ;
	ior->io_Data    = buf;
	ior->io_Length  = 2048;
	ior->io_Offset  = 32768; // Sector 16

	for (int retry = 0; retry < 3; retry++) {
		if ((err = DoIO((struct IORequest*)ior)) == 0) break;
	}

	if (err == 0) {
		// Check ISO ID String & for PVD Version & Type code
		if ((strncmp(iso_id,id_string,5) == 0) && buf[0] == 1 && buf[6] == 1) {
			ret = (strncmp(sys_id_1,system_id,strlen(sys_id_1)) == 0 || strncmp(sys_id_2,system_id,strlen(sys_id_2)) == 0);
		}
	}

done:
	if (buf)  FreeMem(buf,2048);
	return ret;
}

// Build a DeviceNode from a MountFS recipe plus a block extent, load its
// handler, and add it to the mount list. lowCyl/highCyl bound the partition in
// blocks (Surfaces/SectorPerBlock/BlocksPerTrack are all 1, so one block is one
// "cylinder"); pass 0/0 for a whole-medium device such as a CD. Returns 1 on
// success, -1 on failure. The CD and MBR/GPT paths differ only in that extent;
// the RDB path fills its DosEnvec from disk instead and does not use this.
static LONG mount_recipe(struct MountData *md, const struct MountFS *fs,
                         const UBYTE *dosName, LONG bootPri,
                         ULONG lowCyl, ULONG highCyl)
{
	struct ExpansionBase *ExpansionBase = md->ExpansionBase;   /* MakeDosNode base */
	struct ParameterPacket pp;

	memset(&pp, 0, sizeof(pp));
	pp.dosname              = dosName + 1;
	pp.execname             = md->devicename;
	pp.unitnum              = md->unitnum;
	pp.de.de_TableSize      = 16; // up to DE_DOSTYPE
	pp.de.de_SizeBlock      = md->blocksize >> 2;
	pp.de.de_Surfaces       = 1;
	pp.de.de_SectorPerBlock = 1;
	pp.de.de_BlocksPerTrack = 1;
	pp.de.de_LowCyl         = lowCyl;
	pp.de.de_HighCyl        = highCyl;
	pp.de.de_NumBuffers     = fs->buffers ? fs->buffers : 5;
	pp.de.de_BufMemType     = MEMF_ANY|MEMF_CLEAR;
	pp.de.de_MaxTransfer    = fs->maxTransfer ? fs->maxTransfer : 0x100000;
	pp.de.de_Mask           = 0x7FFFFFFE;
	pp.de.de_DosType        = fs->dosType;
	pp.de.de_BootPri        = bootPri;
	if (fs->control) {
		UBYTE *cb = AllocBSTR(fs->control, md->SysBase);   /* lives in the DeviceNode */
		if (cb) {
			pp.de.de_Control   = (ULONG)MKBADDR(cb);
			pp.de.de_TableSize = 19;   // up to de_BootBlocks, includes de_Control
		}
	}

	struct DeviceNode *node = MakeDosNode(&pp);
	if (!node) {
		printf("Could not create DosNode\n");
		return -1;
	}
	if (!SetupFileSystem(md, node, fs)) {
		printf("Could not load filesystem\n");
		return -1;
	}
	AddLegacyNode(md, bootPri, node);
	return 1;
}

// Mount a data CDROM (Amiga-bootable ones get boot priority)
static LONG ScanCDROM(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	const struct MountFS *fs = md->cdFS;
	struct MountFS classicCD;
	UBYTE dosName[DEVNAME_BUFSIZE];
	LONG bootPri;
	LONG isBootable;

	if (!UnitIsReady((struct IOStdReq *)md->request))
		return -1;

	if (!isDataCD((struct IOStdReq *)md->request))
		return -1;

	// "CDTV" or "AMIGA BOOT"?
	isBootable = CheckPVD((struct IOStdReq *)md->request,SysBase);

	if (isBootable == -1) {
		// ISO PVD Not found, RDB CD?
		if (md->flags & MSF_NO_RDB)
			return -1;
		return ScanRDSK(md);
	} else {
		if (isBootable) {
			bootPri = 2; // Yes, give priority
		} else {
			bootPri = -1; // May not be a boot disk, lower priority than HDD
		}
	}

	if (!fs) {
		// No recipe: classic behavior, CD01/CDVD from FileSystem.resource only.
		struct FileSysEntry *fse = find_filesystem(0x43443031, 0x43445644, md->SysBase);
		if (!fse) {
			printf("Could not load filesystem\n");
			return -1;
		}
		memset(&classicCD, 0, sizeof(classicCD));
		classicCD.dosType = fse->fse_DosType; // CD01 / CDVD
		fs = &classicCD;
	} else if (!FileSystemAvailable(md, fs)) {
		printf("No filesystem for dostype 0x%08lx\n", fs->dosType);
		return -1;
	}

	MakeDevName(dosName, fs->dosName ? fs->dosName : (const UBYTE *)"CD0", sizeof(dosName));
	CheckAndFixDevName(md, dosName, sizeof(dosName));

	// Whole-medium mount: the CD filesystem reads the disc directly, so LowCyl/
	// HighCyl are left at 0.
	return mount_recipe(md, fs, dosName, bootPri, 0, 0);
}

/* DetectVBR() results */
#define VBR_NONE  0
#define VBR_FAT   1
#define VBR_NTFS  2
#define VBR_EXFAT 3

// Classify one block as a filesystem Volume Boot Record. A VBR ends in 0x55AA
// just like an MBR, so the BPB fields must be validated to tell a superfloppy
// (filesystem at block 0) from a partition table.
static int DetectVBR(const UBYTE *b, int blocksize)
{
	if (b[510] != 0x55 || b[511] != 0xaa)
		return VBR_NONE;
	if (memcmp(b + 3, "NTFS    ", 8) == 0)
		return VBR_NTFS;
	if (memcmp(b + 3, "EXFAT   ", 8) == 0)
		return VBR_EXFAT;
	if (b[0] != 0xeb && b[0] != 0xe9)          /* x86 jump opcode */
		return VBR_NONE;
	int bps = b[11] | (b[12] << 8);            /* BPB fields are little endian */
	UBYTE spc = b[13];
	if (bps != blocksize)
		return VBR_NONE;
	if (spc == 0 || (spc & (spc - 1)) != 0)    /* sectors/cluster: power of two */
		return VBR_NONE;
	if (b[16] != 1 && b[16] != 2)              /* number of FATs */
		return VBR_NONE;
	return VBR_FAT;
}

static LONG register_legacy(struct MountData *md, UBYTE bootable, UBYTE type, ULONG pstart, ULONG plen)
{
	struct ExecBase *SysBase = md->SysBase;
	const struct MountFS *fs = NULL;
	UBYTE dosName[DEVNAME_BUFSIZE];
	LONG bootPri = bootable ? 0 : -1;
	ULONG pend = pstart + plen - 1;

	if ((md->flags & MSF_LEGACY_FIRST_ONLY) && md->legacyMounted)
		return 0;

	// The MBR type byte / GPT type GUID only got us here; the partition's own
	// boot sector decides the filesystem (0x07 is NTFS or exFAT, sticks are
	// often mislabeled). Sniff it, in a buffer the caller's table doesn't use.
	UBYTE *vbr = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);
	if (!vbr)
		return -1;
	int vbrType = readblock(vbr, pstart, 0xffffffff, md) ? DetectVBR(vbr, md->blocksize) : -1;
	FreeMem(vbr, MAX_BLOCKSIZE);
	if (vbrType < 0)
		return -1;
	switch (vbrType) {
	case VBR_FAT:
		fs = md->fatFS ? md->fatFS : &defaultFatFS;
		break;
	case VBR_NTFS:
		fs = md->ntfsFS;   /* NULL: host mounts no NTFS */
		break;
	default:
		break;
	}
	if (!fs) {
		printf("Skipping partition at %lu (type 0x%02lx): unsupported filesystem\n",
		       pstart, (ULONG)type);
		return 0;
	}
	if (!FileSystemAvailable(md, fs)) {
		printf("Skipping partition at %lu: no filesystem for dostype 0x%08lx\n",
		       pstart, fs->dosType);
		return 0;
	}

	printf("register_legacy: %lu - %lu\n", pstart, pend);

	MakeDevName(dosName, fs->dosName ? fs->dosName : (const UBYTE *)"MS0", sizeof(dosName));
	CheckAndFixDevName(md, dosName, sizeof(dosName));

	// Map the partition's block range straight to LowCyl/HighCyl (one block per
	// cylinder). The old CHS fitting rounded the start down and corrupted
	// unaligned partitions.
	LONG ret = mount_recipe(md, fs, dosName, bootPri, pstart, pend);
	if (ret > 0)
		md->legacyMounted = TRUE;
	return ret;
}

#define MAX_EXTENDED_PARTITIONS 16

// 0x05 = CHS extended, 0x0F = LBA extended (the common one), 0x85 = Linux extended.
static BOOL IsExtendedType(UBYTE type)
{
	return type == 0x05 || type == 0x0f || type == 0x85;
}

// Walk the EBR chain of the extended partition starting at 'base'. Logical
// partition offsets are relative to their own EBR; the next-EBR link is
// relative to the extended container base. Returns partitions mounted.
static LONG parse_extended(struct MountData *md, ULONG base)
{
	struct ExecBase *SysBase = md->SysBase;
	UBYTE *buf = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);
	if (!buf)
		return 0;
	struct mbr *mbr = (struct mbr *)buf;
	ULONG ebr = base;
	LONG mounted = 0;
	int n;

	for (n = 0; n < MAX_EXTENDED_PARTITIONS; n++) {
		if (!readblock(buf, ebr, 0xffffffff, md))
			break;
		if (mbr->sig[0] != 0x55 || mbr->sig[1] != 0xaa)
			break;
		// Slot 0 is the logical partition, slot 1 links to the next EBR. Read
		// them straight from buf — register_legacy() below reads into its own
		// sector, so buf survives intact until the next iteration overwrites it.
		struct mbr_partition *logical = &mbr->part[0];
		struct mbr_partition *link    = &mbr->part[1];

		if (logical->type != 0 && logical->num_sect != 0) {
			printf("   %2ld   ", (LONG)(5 + n));
			printf("%lc   %02lx %8lx %8lx\n", (LONG)(logical->status & 0x80 ? '*':' '),
					(ULONG)logical->type,
					ebr + __bswap32(logical->f_lba),
					(ULONG)__bswap32(logical->num_sect));
			if (register_legacy(md, logical->status & 0x80, logical->type,
					ebr + __bswap32(logical->f_lba),
					__bswap32(logical->num_sect)) > 0)
				mounted++;
		}

		if (!IsExtendedType(link->type) || link->num_sect == 0)
			break;
		ebr = base + __bswap32(link->f_lba);
	}
	if (n == MAX_EXTENDED_PARTITIONS)
		printf("Warning: Extended partition limit (%ld) reached\n",
		       (LONG)MAX_EXTENDED_PARTITIONS);
	FreeMem(buf, MAX_BLOCKSIZE);
	return mounted;
}

// Any 0xEE slot marks the disk GPT, whether protective (covers the whole
// disk) or hybrid (real entries alongside). Per UEFI spec this gates GPT.
static BOOL HasProtectiveEntry(const struct mbr *mbr)
{
	for (int i = 0; i < 4; i++) {
		if (mbr->part[i].type == 0xee)
			return TRUE;
	}
	return FALSE;
}

// CRC-32 (reflected, poly 0xEDB88320), bitwise: only ever run over one ~92-byte
// header, not worth a table.
static ULONG crc32(const UBYTE *p, ULONG len)
{
	ULONG crc = 0xFFFFFFFF;
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
	}
	return ~crc;
}

// UEFI-spec GPT header check: signature, the header must claim to live at the
// LBA it was read from, sane size, and the self-CRC32. This is what tells a
// live GPT from a stale header left at block 1 by an earlier repartitioning.
// (The entry-array CRC is skipped; ParseGPT bounds-checks what it reads.)
static BOOL ValidGPTHeader(struct gpt *gpt, struct MountData *md)
{
	if (memcmp(gpt->signature, "EFI PART", 8) != 0)
		return FALSE;
	if (__bswap64(gpt->my_lba) != 1)
		return FALSE;
	ULONG size = __bswap32(gpt->size);
	if (size < sizeof(struct gpt) || size > (ULONG)md->blocksize)
		return FALSE;
	ULONG stored = gpt->header_crc32;
	gpt->header_crc32 = 0;   /* the CRC is computed with its own field zeroed */
	ULONG computed = crc32((const UBYTE *)gpt, size);
	gpt->header_crc32 = stored;
	return computed == __bswap32(stored);
}

// Reject boot-code garbage that happens to end in 0x55AA (a filesystem VBR does,
// too): every slot must look like a real entry and at least one must be usable.
static BOOL SaneMBR(const struct mbr *mbr, ULONG total)
{
	int used = 0;
	for (int i = 0; i < 4; i++) {
		const struct mbr_partition *p = &mbr->part[i];
		if (p->status & 0x7f)
			return FALSE;
		if (p->type == 0 || p->f_lba == 0 || p->num_sect == 0)
			continue;
		if (total && __bswap32(p->f_lba) >= total)
			return FALSE;
		used++;
	}
	return used > 0;
}

static LONG ParseMBR(UBYTE *buf, struct MountData *md)
{
	struct mbr *mbr = (struct mbr *)buf;
	LONG mounted = 0;

	printf(" Part Boot Type   Start   Length\n");
	// The mount helpers below read into their own sectors, so buf (the MBR)
	// stays valid across the loop — index the four entries in place.
	for (int i = 0; i < 4; i++) {
		struct mbr_partition *p = &mbr->part[i];
		if (p->type == 0 || p->f_lba == 0 || p->num_sect == 0) {
			continue;
		}
		printf("   %2ld   ", (LONG)(i+1));
		printf("%lc   %02lx %8lx %8lx\n", (LONG)(p->status & 0x80 ? '*':' '),
			(ULONG)p->type,
			(ULONG)__bswap32(p->f_lba),
			(ULONG)__bswap32(p->num_sect));

		if (IsExtendedType(p->type)) {
			mounted += parse_extended(md, __bswap32(p->f_lba));
		} else if (p->type == 0xee) {
			// GPT protective entry; GPT itself was already probed at block 1.
		} else {
			if (register_legacy(md, p->status & 0x80, p->type,
					__bswap32(p->f_lba),
					__bswap32(p->num_sect)) > 0)
				mounted++;
		}
	}

	return mounted;
}

static void print_guid(GUID *x)
{
	(void)x; // In case we turned debugging off.

	// Somebody has got to be proud of this mixed endian prank.

	printf("%08lx-%04lx-%04lx-%02lx%02lx-%02lx%02lx%02lx%02lx%02lx%02lx",
			(ULONG)__bswap32(x->u.UUID.time_low), (ULONG)__bswap16(x->u.UUID.time_mid),
			(ULONG)__bswap16(x->u.UUID.time_high_and_version),
			(ULONG)x->u.UUID.clock_seq_high_and_reserved, (ULONG)x->u.UUID.clock_seq_low,
			(ULONG)x->u.UUID.node[0], (ULONG)x->u.UUID.node[1], (ULONG)x->u.UUID.node[2],
			(ULONG)x->u.UUID.node[3], (ULONG)x->u.UUID.node[4], (ULONG)x->u.UUID.node[5]);
}

// Microsoft Basic Data Partition GUID (used for FAT and NTFS)
// EBD0A0A2-B9E5-4433-87C0-68B6B72699C7
static const GUID GUID_BASIC_DATA = {{
	{ 0xEBD0A0A2, 0xB9E5, 0x4433, 0x87, 0xC0,
	  { 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 } }
}};

static int guid_equal(const GUID *a, const GUID *b)
{
	return memcmp(a->u.raw, b->u.raw, 16) == 0;
}

static LONG ParseGPT(UBYTE *hdr, struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	const struct gpt *gpt=(struct gpt *)hdr;
	uint64_t pstart = __bswap64(gpt->entries_lba);
	int numparts = __bswap32(gpt->number_of_entries);
	const int psize = __bswap32(gpt->size_of_entry);
	LONG mounted = 0;

	printf(" Number of partitions: %ld\n", (LONG)numparts);
	printf(" size of entry: %ld\n", (LONG)psize);

	if (psize < (int)sizeof(struct gpt_partition) || psize > md->blocksize)
		return -1;
	if (numparts > 128)   /* standard table size; don't chase garbage counts */
		numparts = 128;
	const int per_block = md->blocksize / psize;

	// The partition-entry array is read into its own sector; the header stays
	// in 'hdr' (its fields are still needed) and block 0 is left untouched.
	UBYTE *buf = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);
	if (!buf)
		return -1;

	for (int i = 0, pos = 0; i < numparts; i++) {
		if (pos == 0) {
			if (!readblock(buf, (ULONG)pstart++, 0xffffffff, md))
				break;
		}
		struct gpt_partition *gpt_par = (struct gpt_partition *)(buf + (pos * psize));
		if (++pos == per_block)
			pos = 0;

		/* skip empty partitions */
		if (gpt_par->first_lba == 0 && gpt_par->last_lba == 0)
			continue;

		const uint64_t first_lba = __bswap64(gpt_par->first_lba);
		const uint64_t last_lba = __bswap64(gpt_par->last_lba);

		printf("%ld. %08lx%08lx - %08lx%08lx ", (LONG)i,
				(ULONG)(first_lba >> 32), (ULONG)first_lba,
				(ULONG)(last_lba >> 32), (ULONG)last_lba);
		print_guid(&gpt_par->partition_type);
		printf("\n");

		if (!guid_equal(&gpt_par->partition_type, &GUID_BASIC_DATA)) {
			printf("   Skipping non-FAT partition type\n");
			continue;
		}
		if ((last_lba >> 32) != 0) {
			printf("   Skipping partition beyond 2^32 blocks\n");
			continue;
		}
		if (register_legacy(md, 0, 0, (ULONG)first_lba,
				(ULONG)(last_lba - first_lba + 1)) > 0)
			mounted++;
	}

	FreeMem(buf, MAX_BLOCKSIZE);
	return mounted;
}

// Non-RDB media, classified from one read of block 0: GPT (gated by its
// protective/hybrid MBR entry, so a stale GPT header on a since-repartitioned
// disk can't override the current table), then a filesystem straight at
// block 0 (superfloppy — its VBR ends in 0x55AA too, so it must be checked
// before the MBR signature), then MBR.
// Returns -1 if nothing was recognized, else partitions mounted.
static LONG ScanLegacy(struct MountData *md)
{
	struct ExecBase *SysBase = md->SysBase;
	UBYTE *buf = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);
	if (!buf)
		return -1;
	LONG ret = -1;

	if (readblock(buf, 0, 0xffffffff, md)) {
		struct mbr *mbr = (struct mbr *)buf;
		BOOL hasMBRSig = mbr->sig[0] == 0x55 && mbr->sig[1] == 0xaa;

		if (hasMBRSig && HasProtectiveEntry(mbr)) {
			// The GPT header gets its own sector so block 0 stays intact for the
			// MBR fallback if the header is invalid. A valid header commits us to
			// GPT — ParseGPT's result stands even when it mounts nothing.
			UBYTE *hdr = AllocMem(MAX_BLOCKSIZE, MEMF_PUBLIC);
			BOOL gptValid = FALSE;
			if (hdr) {
				if (readblock(hdr, 1, 0xffffffff, md) &&
				    ValidGPTHeader((struct gpt *)hdr, md)) {
					dbg("GPT found\n");
					gptValid = TRUE;
					ret = ParseGPT(hdr, md);
				}
				FreeMem(hdr, MAX_BLOCKSIZE);
			}
			if (gptValid)
				goto done;
			// Fall through: a hybrid MBR may still carry mountable entries; a
			// pure protective one fails SaneMBR or mounts nothing in ParseMBR.
			printf("GPT protective entry but no valid GPT header at block 1\n");
		}

		if (DetectVBR(buf, md->blocksize) != VBR_NONE) {
			if (md->totalsectors) {
				dbg("Superfloppy (no partition table)\n");
				ret = register_legacy(md, 0, 0, 0, md->totalsectors) > 0 ? 1 : 0;
			}
		} else if (hasMBRSig && SaneMBR(mbr, md->totalsectors)) {
			dbg("MBR found\n");
			ret = ParseMBR(buf, md);
		}
	}

done:
	FreeMem(buf, MAX_BLOCKSIZE);
	return ret;
}

// Probe one unit: open it, read geometry, scan its partition table(s), close.
// Returns the per-unit result (-1 error/no RDB, 0 none, >0 partitions mounted).
static LONG ProbeUnit(struct MountData *md, struct MountStruct *ms, ULONG unitNum,
                      struct IOExtTD *request)
{
	struct ExecBase *SysBase = md->SysBase;
	struct DriveGeometry geom;
	LONG ret = -1;
	UBYTE err;

	dbg("OpenDevice('%s', %"PRId32", 0x%08lx, 0)\n", ms->deviceName, unitNum, (ULONG)request);
	err = OpenDevice(ms->deviceName, unitNum, (struct IORequest*)request, 0);
	if (err != 0) {
		dbg("OpenDevice(%s,%"PRId32") failed: %"PRId32"\n", ms->deviceName, unitNum, (BYTE)err);
		return -1;
	}
	if (GetGeometry(request, &geom) == 0) {
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
		switch (geom.dg_DeviceType & SID_TYPE) {
		case DG_CDROM:
		case DG_WORM:
		case DG_OPTICAL_DISK:
			if (!(md->flags & MSF_NO_CD))
				ret = ScanCDROM(md);
			else
				printf("CDROM mounting disabled.\n");
			break;
		case DG_DIRECT_ACCESS:
			if (!(md->flags & MSF_NO_RDB))
				ret = ScanRDSK(md);
			if (ret == -1 && !(md->flags & MSF_NO_LEGACY))
				ret = ScanLegacy(md);
			break;
		default:
			printf("Don't know how to boot from device type %ld.\n", (LONG)(geom.dg_DeviceType & SID_TYPE));
			break;
		}
	}
out:
	// Disable motor after probing (md->request is unset if geometry failed)
	request->iotd_Req.io_Command = TD_MOTOR;
	request->iotd_Req.io_Length  = 0;
	DoIO((struct IORequest*)request);
	CloseDevice((struct IORequest*)request);
	return ret;
}

// Full SCSI-style scan: every target 0-7 (skipping the host ID), optionally each
// target's LUNs, honoring RDBFF_LASTLUN / RDBFF_LAST. Accumulates the mounted
// count into *total and sets *recognized if any unit held recognizable media.
static void ScanAllUnits(struct MountData *md, struct MountStruct *ms,
                         struct IOExtTD *request, LONG *total, BOOL *recognized)
{
	for (ULONG target = 0; target < 8; target++) {
		if (target == ms->hostId)   // skip the host controller ID
			continue;
		ULONG lun = 0;
		for (;;) {
			ULONG unitNum;
			if (target > 7 || lun > 7)
				unitNum = lun * 10 * 1000 + target * 10 + HD_WIDESCSI;  // Phase V wide SCSI
			else
				unitNum = target + lun * 10;                            // traditional scheme
			LONG r = ProbeUnit(md, ms, unitNum, request);
			if (r >= 0) {
				*recognized = TRUE;
				*total += r;
			}
			if (!(ms->luns && lun++ < 8 && !md->wasLastLun))
				break;
		}
		if (md->wasLastDev && !ms->ignoreLast) {
			dbg("RDBFF_LAST exit\n");
			break;
		}
	}
}

// Explicit unit(s): a single unit number (< 0x100), else a pointer to a
// { count, unit0, unit1, ... } array. Lets a caller mount known units (e.g. a
// hotplug driver) instead of scanning. For an array, each entry is overwritten
// with that unit's result (-2 = skipped after a prior RDBFF_LAST).
static void ScanUnitList(struct MountData *md, struct MountStruct *ms,
                         struct IOExtTD *request, LONG *total, BOOL *recognized)
{
	ULONG single[2];
	ULONG *list;
	if ((ULONG)ms->unitNum < 0x100) {
		single[0] = 1;
		single[1] = (ULONG)ms->unitNum;
		list = single;
	} else {
		list = ms->unitNum;
	}
	ULONG n = list[0];
	BOOL skipRest = FALSE;
	for (ULONG i = 1; i <= n; i++) {
		LONG r = -2;
		if (!skipRest) {
			r = ProbeUnit(md, ms, list[i], request);
			if (r >= 0) {
				*recognized = TRUE;
				*total += r;
			}
			if (md->wasLastDev && !ms->ignoreLast) {
				dbg("RDBFF_LAST exit\n");
				skipRest = TRUE;
			}
		}
		if (list != single)
			list[i] = r;
	}
}

// Return value: total number of partitions mounted across all units (>0);
// 0 if at least one unit carried a recognized medium but nothing was mounted;
// -1 if no partition table / filesystem was recognized on any unit.
// If a unit number array was passed, each unit number is additionally replaced
// with that unit's result: -1 = nothing recognized (or device failed to open),
// 0 = recognized but nothing mounted, >0 = partitions mounted,
// -2 = skipped because a previous unit had RDBFF_LAST set.
LONG MountDrive(struct MountStruct *ms)
{
	struct ExecBase *SysBase = ms->SysBase;
	struct ExpansionBase *ExpansionBase = NULL;
	struct MountData *md = NULL;
	struct MsgPort *port = NULL;
	struct IOExtTD *request = NULL;
	LONG total = 0;
	BOOL recognized = FALSE;

	dbg("Starting..\n");

	ExpansionBase = (struct ExpansionBase*)OpenLibrary("expansion.library", 34);
	if (!ExpansionBase)
		goto cleanup;

	md = AllocMem(sizeof(struct MountData), MEMF_CLEAR | MEMF_PUBLIC);
	if (!md)
		goto cleanup;

	md->DOSBase = (struct DosLibrary*)OpenLibrary("dos.library", 34);
	md->SysBase = SysBase;
	md->ExpansionBase = ExpansionBase;
	dbg("SysBase=0x%08lx ExpansionBase=0x%08lx DosBase=0x%08lx\n", (ULONG)md->SysBase, (ULONG)md->ExpansionBase, (ULONG)md->DOSBase);
	md->configDev = ms->configDev;
	md->creator = ms->creatorName;
	md->slowSpinup = ms->slowSpinup;
	md->flags = ms->flags;
	md->fatFS = ms->fatFS;
	md->ntfsFS = ms->ntfsFS;
	md->cdFS = ms->cdFS;

	port = W_CreateMsgPort(SysBase);
	if (!port)
		goto cleanup;

	request = (struct IOExtTD*)W_CreateIORequest(port, sizeof(struct IOExtTD), SysBase);
	if (!request)
		goto cleanup;

	if (ms->unitNum == NULL)
		ScanAllUnits(md, ms, request, &total, &recognized);
	else
		ScanUnitList(md, ms, request, &total, &recognized);

cleanup:
	if (request)
		W_DeleteIORequest(request, SysBase);
	if (port)
		W_DeleteMsgPort(port, SysBase);
	if (md) {
		if (md->DOSBase)
			CloseLibrary(&md->DOSBase->dl_lib);
		FreeMem(md, sizeof(struct MountData));
	}
	if (ExpansionBase)
		CloseLibrary(&ExpansionBase->LibNode);

	LONG ret = total > 0 ? total : (recognized ? 0 : -1);
	dbg("Exit code %"PRId32"\n", ret);
	return ret;
}
