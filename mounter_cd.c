
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

/**
 * is_unit_ready() - Check if there is a disc inserted.
 * @md:  mount state; only md->SysBase is used
 * @req: the unit's open request (md->request), DoIO()'d; io_Command, io_Error
 *       and io_Actual are overwritten
 *
 * Sends CMD_START first to spin the disc up, ignoring its error, then
 * TD_CHANGESTATE.
 *
 * Return: TRUE if TD_CHANGESTATE reports a disc (io_Actual == 0) or the device
 * does not support it (IOERR_NOCMD, taken as ready); FALSE if it reports no
 * disc or fails otherwise
 */
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
enum CdDisc
{
	CDDISC_UNKNOWN,     /* TOC unreadable/implausible (blank disc, drive error) */
	CDDISC_DATA,        /* track 1 is a data track */
	CDDISC_AUDIO,       /* track 1 is an audio track */
};

/**
 * classify_cd() - Classify the disc by reading the TOC and checking track 1's data-track bit.
 * @md:  mount state; the TOC is read into one of its scratch sectors
 * @ior: the unit's open request, DoIO()'d with HD_SCSICMD; io_Command, io_Data
 *       and io_Length are overwritten
 *
 * Sends READ TOC (format 0, LBA, from track 1) up to three times, until both
 * io_Error and scsi_Status are 0. The data-track bit is 0x04 of the track
 * descriptor's control nibble.
 *
 * Return: CDDISC_DATA or CDDISC_AUDIO by that bit; CDDISC_UNKNOWN if the TOC
 * cannot be read (no scratch sector, I/O error or CHECK CONDITION on every try)
 * or does not start at track 1
 */
static enum CdDisc classify_cd(struct MountData *md, struct IOStdReq *ior)
{
	struct ExecBase *SysBase = md->SysBase;
	enum CdDisc ret = CDDISC_UNKNOWN;
	_Static_assert(sizeof(struct SCSI_CD_TOC) <= MAX_BLOCKSIZE, "TOC in a scratch sector");

	struct SCSI_CD_TOC *toc = (struct SCSI_CD_TOC *)mnt_sector_take(md);
	if (!toc)
		return ret;
	// What is read below: a TOC cut short leaves these as zeros.
	memset(toc, 0, offsetof(struct SCSI_CD_TOC, td[1]));

	const ULONG bufSize = sizeof(struct SCSI_CD_TOC);
	UBYTE cdb[10] = {
		[0] = SCSI_CMD_READ_TOC,
		[2] = 0,                     // Format: 0
		[6] = 1,                     // Track 1
		[7] = (UBYTE)(bufSize >> 8),
		[8] = (UBYTE)(bufSize & 0xFF),
	};
	struct SCSICmd scsiCmd = {
		.scsi_Data      = (UWORD *)toc,
		.scsi_Length    = bufSize,
		.scsi_Flags     = SCSIF_READ,
		.scsi_CmdLength = sizeof(cdb),
		.scsi_Command   = cdb,
	};

	ior->io_Data    = &scsiCmd;
	ior->io_Length  = sizeof(struct SCSICmd);
	ior->io_Command = HD_SCSICMD;

	BYTE err = 0;
	for (int retry = 0; retry < 3; retry++) {
		if ((err = DoIO((struct IORequest *)ior)) == 0 && scsiCmd.scsi_Status == 0)
			break;
	}

	// The loop also falls out on exhausted retries, so re-test what it was
	// waiting for. io_Error alone is not enough: a CHECK CONDITION completes
	// the request cleanly (err == 0) and reports the drive's refusal in
	// scsi_Status.
	if (err == 0 && scsiCmd.scsi_Status == 0) {
		if (toc->firstTrack == 1 && toc->td[0].trackNumber == 1) {
			// Data track bit
			ret = (toc->td[0].adrControl & 0x04) ? CDDISC_DATA : CDDISC_AUDIO;
		}
	}

	mnt_sector_drop(md, (UBYTE *)toc);
	return ret;
}

// read_pvd results, in order: everything below PVD_DATA is "no ISO9660 PVD".
// Only PVD_AMIGABOOT decides anything about the disc's contents; the rest tell
// mnt_scan_cd how much it may assume. PVD_NONE is not a rejection - plenty of
// formats the CD handler reads (High Sierra, plain UDF, HFS/HFS+) have nothing
// at all at sector 16 offset 1.
enum Pvd
{
	PVD_ERROR = -2,     /* sector 16 unreadable, or no buffer to read it into */
	PVD_NONE,           /* readable, but no ISO9660 PVD */
	PVD_DATA,           /* ISO9660, not Amiga-bootable */
	PVD_AMIGABOOT,      /* ISO9660 with "CDTV" or "AMIGA BOOT" as the System ID */
};

// Byte offset of the ISO9660 Primary Volume Descriptor: 2048-byte sector 16.
#define PVD_OFFSET 32768

// The first fields of an ISO 9660 volume descriptor (ECMA-119 8.4), all that
// read_pvd() looks at. Bytes only, so there is no padding.
struct IsoVolumeDescriptor
{
	UBYTE type;             /* 1: the Primary Volume Descriptor */
	char  standardId[5];    /* "CD001" in every ISO 9660 volume descriptor */
	UBYTE version;          /* 1 */
	UBYTE unused;
	char  systemId[32];     /* the system that may use the disc's first 16 sectors; space-padded */
};
_Static_assert(offsetof(struct IsoVolumeDescriptor, systemId) == 8, "ISO 9660 descriptor layout");

/**
 * id_starts_with() - Tell whether an ISO 9660 identifier field begins with a text.
 * @id:     the field; space-padded, not NUL-terminated
 * @prefix: the text, a C string no longer than the field
 *
 * Return: TRUE if the field's first strlen(@prefix) chars are @prefix; FALSE
 * otherwise
 */
static BOOL id_starts_with(const char *id, const char *prefix)
{
	return memcmp(id, prefix, strlen(prefix)) == 0;
}

/**
 * read_pvd() - Read the ISO9660 Primary Volume Descriptor to decide boot priority.
 * @md: mount state of the open unit; the block holding byte PVD_OFFSET is read
 *      (mnt_read_block()) into one of its scratch sectors
 *
 * Is the System ID "CDTV" or "AMIGA BOOT"? Identifying the disc's format is the
 * CD filesystem's job, not ours. The fields read lie in the descriptor's first 40
 * bytes, so one block of any size holds them.
 *
 * Return: PVD_AMIGABOOT if sector 16 is an ISO9660 PVD (type 1, "CD001",
 * version 1) whose System ID starts with "CDTV" or "AMIGA BOOT"; PVD_DATA for
 * any other ISO9660 PVD; PVD_NONE if the sector reads but holds no PVD;
 * PVD_ERROR if it cannot be read, PVD_OFFSET is not on a block boundary, or no
 * scratch sector is free
 */
static enum Pvd read_pvd(struct MountData *md)
{
	if (PVD_OFFSET % md->blocksize)
		return PVD_ERROR;
	UBYTE *buf = mnt_sector_take(md);
	if (!buf)
		return PVD_ERROR;
	enum Pvd ret = PVD_ERROR;

	if (mnt_read_block(buf, PVD_OFFSET / md->blocksize, md)) {
		const struct IsoVolumeDescriptor *pvd = (const struct IsoVolumeDescriptor *)buf;
		// An ISO 9660 disc has its Primary Volume Descriptor here: type 1,
		// "CD001", version 1. Anything else is a disc of another format, for
		// the CD filesystem to judge. An Amiga-bootable disc names itself in
		// the System Identifier, as "AMIGA BOOT" or "CDTV".
		if (pvd->type != 1 || memcmp(pvd->standardId, "CD001", 5) != 0 || pvd->version != 1)
			ret = PVD_NONE;
		else if (id_starts_with(pvd->systemId, "AMIGA BOOT") || id_starts_with(pvd->systemId, "CDTV"))
			ret = PVD_AMIGABOOT;
		else
			ret = PVD_DATA;
	}

	mnt_sector_drop(md, buf);
	return ret;
}

/**
 * mnt_scan_cd() - Mount a CDROM as one whole-medium volume.
 * @md: mount state of the open unit; md->fs[MOUNTFS_CD] is the recipe, never
 *      NULL (MountDrive() fills in the classic CD01/CDVD one)
 *
 * Amiga-bootable data discs get boot priority: 5, else MOUNT_NEVER_BOOT (an
 * RDB CD's partitions follow their RDB). What format the disc carries is the CD
 * recipe's filesystem's business; the recipe declares what it can cope with.
 * MOUNTFS_CD_ANYFMT means it identifies formats itself (e.g. ODFileSystem: High
 * Sierra, UDF, HFS/HFS+ besides ISO9660), MOUNTFS_CD_AUDIO that it can present
 * audio-only discs (ODFileSystem exposes the tracks as WAV files). Without
 * those a data disc has to be ISO9660 and an audio disc is refused, which is
 * all a legacy CDFileSystem can do. A data disc without an ISO9660 PVD is tried
 * as an RDB CD (mnt_scan_rdb()) first, unless MSF_NO_RDB.
 *
 * Return: TRUE if the disc was recognized: an RDB on it, or a data or audio
 * disc the recipe takes, whether or not its volume then mounts (md->mounted
 * counts it; md->deferred one whose filesystem waits for DOS). FALSE if there is
 * no disc or the disc is refused
 */
BOOL mnt_scan_cd(struct MountData *md)
{
	const struct MountFS *fs = md->fs[MOUNTFS_CD];
	LONG bootPri = MOUNT_NEVER_BOOT;

	if (!is_unit_ready(md, (struct IOStdReq *)md->request))
		return FALSE;

	enum CdDisc disc = classify_cd(md, (struct IOStdReq *)md->request);

	// Some enclosures answer READ TOC poorly for DVD/BD media - which is
	// exactly the media UDF lives on. Give a self-identifying filesystem the
	// benefit of the doubt; sector 16 still has to read back below.
	if (disc == CDDISC_UNKNOWN && (fs->fsFlags & MOUNTFS_CD_ANYFMT))
		disc = CDDISC_DATA;

	switch (disc) {
	case CDDISC_DATA:
	{
		// "CDTV" or "AMIGA BOOT"?
		enum Pvd pvd = read_pvd(md);

		if (pvd < PVD_DATA) {
			// No ISO9660 PVD. RDB CD?
			if (!(md->flags & MSF_NO_RDB) && mnt_scan_rdb(md)) {
				return TRUE;
			}
			// Not RDB either. A filesystem that identifies formats
			// itself still reads this disc; a PVD-only one does not,
			// and an unreadable sector 16 is nobody's disc.
			if (pvd == PVD_ERROR || !(fs->fsFlags & MOUNTFS_CD_ANYFMT)) {
				mnt_log("Unrecognized disc.\n");
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
		if (!(fs->fsFlags & MOUNTFS_CD_AUDIO)) {
			mnt_log("Audio disc and no audio-capable CD filesystem.\n");
			return FALSE;
		}
		break;
	case CDDISC_UNKNOWN:
		mnt_log("Unrecognized disc TOC.\n");
		return FALSE;
	}

	// From here on the disc is recognized, whatever becomes of its volume: a
	// filesystem that has to wait for DOS, or none at all, mounts nothing.
	// Whole-medium mount: the CD filesystem reads the disc directly, so its
	// first and last block stay 0.
	struct Volume vol = {
		.fs       = fs,
		.nameHint = fs->dosName ? fs->dosName : (const UBYTE *)"CD0",
		.bootPri  = bootPri,
	};
	mnt_mount_volume(md, &vol);
	return TRUE;
}
