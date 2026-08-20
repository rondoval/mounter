
// mounter_cd.c — CD/DVD/BD: readiness, TOC classification, ISO9660 PVD.
//
// Part of the generic autoboot/automount mounter; see mounter.c for the
// copyright and licence covering this file.
#include "mounter_internal.h"

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

// Check if there is a disc inserted
static BOOL is_unit_ready(struct MountData *md, struct IOStdReq *req)
{
	struct ExecBase *SysBase = md->SysBase;

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
	if (err == IOERR_NOCMD) return TRUE;

	if (err == 0 && req->io_Actual == 0) return TRUE;

	return FALSE;
}


// Disc classes derived from the TOC.
#define CDDISC_UNKNOWN 0	// TOC unreadable/implausible (blank disc, drive error)
#define CDDISC_DATA    1	// track 1 is a data track
#define CDDISC_AUDIO   2	// track 1 is an audio track

// Classify the disc by reading the TOC and checking track 1's data-track bit.
static int classify_cd(struct MountData *md, struct IOStdReq *ior)
{
	struct ExecBase *SysBase = md->SysBase;
	int ret = CDDISC_UNKNOWN;

	BYTE err;

	struct SCSICmd     *scsiCmd = NULL;
	struct SCSI_CD_TOC *tocBuf  = NULL;

	ULONG bufSize = sizeof(struct SCSI_CD_TOC);

	UBYTE cdb[10];
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
			cdb[7] = (UBYTE)(bufSize >> 8);
			cdb[8] = (UBYTE)(bufSize & 0xFF);

			ior->io_Data    = scsiCmd;
			ior->io_Length  = sizeof(struct SCSICmd);
			ior->io_Command = HD_SCSICMD;

			for (int retry = 0; retry < 3; retry++) {
				if ((err = DoIO((struct IORequest *)ior)) == 0 && scsiCmd->scsi_Status == 0)
					break;
			}

			// The loop also falls out on exhausted retries, so re-test
			// what it was waiting for. io_Error alone is not enough: a
			// CHECK CONDITION completes the request cleanly (err == 0)
			// and reports the drive's refusal in scsi_Status, leaving
			// the MEMF_CLEAR TOC buffer at zeros.
			if (err == 0 && scsiCmd->scsi_Status == 0) {
				if (tocBuf->firstTrack == 1 && tocBuf->td[0].trackNumber == 1) {
					// Data track bit
					ret = (tocBuf->td[0].adrControl & 0x04) ? CDDISC_DATA : CDDISC_AUDIO;
				}
			}

			FreeMem(tocBuf,bufSize);
		}
		FreeMem(scsiCmd,sizeof(struct SCSICmd));
	}
	return ret;
}

// read_pvd results. Only PVD_AMIGABOOT decides anything about the disc's
// contents; the rest tell mnt_scan_cd how much it may assume. PVD_NONE is not a
// rejection - plenty of formats the CD handler reads (High Sierra, plain UDF,
// HFS/HFS+) have nothing at all at sector 16 offset 1.
#define PVD_ERROR      -2	// sector 16 unreadable
#define PVD_NONE       -1	// readable, but no ISO9660 PVD
#define PVD_DATA        0	// ISO9660, not Amiga-bootable
#define PVD_AMIGABOOT   1	// ISO9660 with "CDTV" or "AMIGA BOOT" as the System ID

// read_pvd
// Read the ISO9660 Primary Volume Descriptor to decide boot priority: is the
// System ID "CDTV" or "AMIGA BOOT"? Identifying the disc's format is the CD
// filesystem's job, not ours.
// Returns: one of the PVD_* values above.
static LONG read_pvd(struct MountData *md, struct IOStdReq *ior)
{
	struct ExecBase *SysBase = md->SysBase;
	const char sys_id_1[] = "CDTV";
	const char sys_id_2[] = "AMIGA BOOT";
	const char iso_id[]   = "CD001";

	BYTE err = 0;
	LONG ret = PVD_ERROR;
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
		ret = PVD_NONE;
		// Check ISO ID String & for PVD Version & Type code
		if ((strncmp(iso_id,id_string,5) == 0) && buf[0] == 1 && buf[6] == 1) {
			ret = (strncmp(sys_id_1,system_id,strlen(sys_id_1)) == 0 || strncmp(sys_id_2,system_id,strlen(sys_id_2)) == 0)
			      ? PVD_AMIGABOOT : PVD_DATA;
		}
	}

done:
	if (buf)  FreeMem(buf,2048);
	return ret;
}

// Mount a CDROM (Amiga-bootable data discs get boot priority). What format the
// disc carries is the CD recipe's filesystem's business; the recipe declares
// what it can cope with. MOUNTFS_CD_ANYFMT means it identifies formats itself (e.g.
// ODFileSystem: High Sierra, UDF, HFS/HFS+ besides ISO9660), MOUNTFS_CD_AUDIO that
// it can present audio-only discs (ODFileSystem exposes the tracks as WAV
// files). Without those a data disc has to be ISO9660 and an audio disc is
// refused, which is all a legacy CDFileSystem can do.
// Returns TRUE if a mountable disc was recognized; the count is in md->mounted.
BOOL mnt_scan_cd(struct MountData *md)
{
	const struct MountFS *fs = md->fs[MOUNTFS_CD];
	struct MountFS classicCD;
	LONG bootPri = MOUNT_NEVER_BOOT;

	if (!is_unit_ready(md, (struct IOStdReq *)md->request))
		return FALSE;

	int disc = classify_cd(md, (struct IOStdReq *)md->request);

	// Some enclosures answer READ TOC poorly for DVD/BD media - which is
	// exactly the media UDF lives on. Give a self-identifying filesystem the
	// benefit of the doubt; sector 16 still has to read back below.
	if (disc == CDDISC_UNKNOWN && fs && (fs->fsFlags & MOUNTFS_CD_ANYFMT))
		disc = CDDISC_DATA;

	switch (disc) {
	case CDDISC_DATA:
	{
		// "CDTV" or "AMIGA BOOT"?
		LONG pvd = read_pvd(md, (struct IOStdReq *)md->request);

		if (pvd < PVD_DATA) {
			// No ISO9660 PVD. RDB CD?
			if (!(md->flags & MSF_NO_RDB) && mnt_scan_rdb(md)) {
				return TRUE;
			}
			// Not RDB either. A filesystem that identifies formats
			// itself still reads this disc; a PVD-only one does not,
			// and an unreadable sector 16 is nobody's disc.
			if (pvd == PVD_ERROR || !fs || !(fs->fsFlags & MOUNTFS_CD_ANYFMT)) {
				printf("Unrecognized disc.\n");
				return FALSE;
			}
			// Mountable, but nothing on it claims to be bootable.
		} else if (pvd == PVD_AMIGABOOT) {
			bootPri = 5; // Yes, give priority
		}
		break;
	}
	case CDDISC_AUDIO:
		// No PVD to check: audio sectors are not readable via CMD_READ.
		if (!fs || !(fs->fsFlags & MOUNTFS_CD_AUDIO)) {
			printf("Audio disc and no audio-capable CD filesystem.\n");
			return FALSE;
		}
		break;
	default:
		printf("Unrecognized disc TOC.\n");
		return FALSE;
	}

	if (!fs) {
		// No recipe: classic behavior, CD01/CDVD from FileSystem.resource only.
		struct FileSysEntry *fse = mnt_find_filesystem(0x43443031, 0x43445644, md->SysBase);
		if (!fse) {
			printf("Could not load filesystem\n");
			return FALSE;
		}
		memset(&classicCD, 0, sizeof(classicCD));
		classicCD.dosType = fse->fse_DosType; // CD01 / CDVD
		fs = &classicCD;
	} else if (!mnt_resolve_fs(md, fs, 0)) {
		return FALSE;
	}

	// Whole-medium mount: the CD filesystem reads the disc directly, so LowCyl/
	// HighCyl are left at 0.
	struct Volume vol = {
		.fs       = fs,
		.nameHint = fs->dosName ? fs->dosName : (const UBYTE *)"CD0",
		.bootPri  = bootPri,
	};
	mnt_envec_from_recipe(md, &vol.pp.de, fs, 0, 0, bootPri);
	return mnt_mount_volume(md, &vol) != MOUNT_FAILED;
}
