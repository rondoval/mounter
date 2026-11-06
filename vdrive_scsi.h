/*
 * vdrive_scsi.h — the SCSI wire formats the virtual units' SCSI target builds
 * and the parent fence reads: operation codes, status and sense codes, the CDBs
 * and parameter lists they take and the data they return, as SPC-3, SBC-3 and
 * SAT lay them out.
 *
 * Every structure is packed and in the CPU's own byte order, which on a 68k is
 * the wire's: a field reads and writes as it is. Packed also has the compiler
 * reach a field byte by byte where the CPU traps on an odd address (a 68000 or
 * 68010), since a CDB and the buffers a client hands over may sit anywhere.
 *
 * Part of the generic autoboot/automount mounter; see mounter.c for the
 * copyright and licence covering this file.
 */
#ifndef VDRIVE_SCSI_H
#define VDRIVE_SCSI_H

#include <exec/types.h>
#include <stdint.h>

/* ---- operation codes: a CDB's first byte -------------------------------- */

#define SCSI_TEST_UNIT_READY            0x00
#define SCSI_REQUEST_SENSE              0x03
#define SCSI_READ_6                     0x08
#define SCSI_WRITE_6                    0x0a
#define SCSI_INQUIRY                    0x12
#define SCSI_MODE_SENSE_6               0x1a
#define SCSI_START_STOP_UNIT            0x1b
#define SCSI_RECEIVE_DIAGNOSTIC         0x1c
#define SCSI_PREVENT_ALLOW_REMOVAL      0x1e
#define SCSI_READ_FORMAT_CAPACITIES     0x23
#define SCSI_READ_CAPACITY_10           0x25
#define SCSI_READ_10                    0x28
#define SCSI_WRITE_10                   0x2a
#define SCSI_WRITE_VERIFY_10            0x2e
#define SCSI_VERIFY_10                  0x2f
#define SCSI_PREFETCH_10                0x34
#define SCSI_SYNCHRONIZE_CACHE_10       0x35
#define SCSI_READ_DEFECT_DATA_10        0x37
#define SCSI_READ_BUFFER                0x3c
#define SCSI_WRITE_SAME_10              0x41
#define SCSI_UNMAP                      0x42
#define SCSI_READ_TOC                   0x43    /* READ TOC/PMA/ATIP */
#define SCSI_GET_CONFIGURATION          0x46
#define SCSI_GET_EVENT_STATUS           0x4a    /* GET EVENT STATUS NOTIFICATION */
#define SCSI_LOG_SENSE                  0x4d
#define SCSI_READ_DISC_INFORMATION      0x51
#define SCSI_MODE_SENSE_10              0x5a
#define SCSI_ATA_PASS_THROUGH_16        0x85
#define SCSI_READ_16                    0x88
#define SCSI_WRITE_16                   0x8a
#define SCSI_WRITE_VERIFY_16            0x8e
#define SCSI_VERIFY_16                  0x8f
#define SCSI_SYNCHRONIZE_CACHE_16       0x91
#define SCSI_WRITE_SAME_16              0x93
#define SCSI_SERVICE_ACTION_IN_16       0x9e
#define SCSI_REPORT_LUNS                0xa0
#define SCSI_ATA_PASS_THROUGH_12        0xa1
#define SCSI_MAINTENANCE_IN             0xa3
#define SCSI_READ_12                    0xa8
#define SCSI_WRITE_12                   0xaa
#define SCSI_WRITE_VERIFY_12            0xae
#define SCSI_VERIFY_12                  0xaf
#define SCSI_READ_DEFECT_DATA_12        0xb7
#define SCSI_MECHANISM_STATUS           0xbd

/* A CDB's group code, opcode bits 7-5: it gives the CDB's length. */
#define SCSI_GROUP(opcode)              ((opcode) >> 5)
#define SCSI_GROUPS                     8

/* ---- status and sense ------------------------------------------------------ */

#define SCSI_STATUS_GOOD                0x00
#define SCSI_STATUS_CHECK_CONDITION     0x02

#define SCSI_KEY_NO_SENSE               0x00
#define SCSI_KEY_NOT_READY              0x02
#define SCSI_KEY_MEDIUM_ERROR           0x03
#define SCSI_KEY_ILLEGAL_REQUEST        0x05
#define SCSI_KEY_DATA_PROTECT           0x07
#define SCSI_KEY_ABORTED_COMMAND        0x0b

/* Additional sense codes and qualifiers, as ASC << 8 | ASCQ. */
#define SCSI_ASC_NONE                   0x0000
#define SCSI_ASC_WRITE_ERROR            0x0c00
#define SCSI_ASC_UNRECOVERED_READ       0x1100  /* unrecovered read error */
#define SCSI_ASC_INVALID_OPCODE         0x2000  /* invalid command operation code */
#define SCSI_ASC_LBA_OUT_OF_RANGE       0x2100
#define SCSI_ASC_INVALID_FIELD_IN_CDB   0x2400
#define SCSI_ASC_WRITE_PROTECTED        0x2700
#define SCSI_ASC_MEDIUM_NOT_PRESENT     0x3a00
#define SCSI_ASC_NO_RESOURCES           0x5503  /* insufficient resources */

#define SCSI_SENSE_CURRENT              0x70    /* response code: fixed format, current error */

/* Fixed-format sense data. */
struct SCSISense
{
	UBYTE responseCode;
	UBYTE obsolete;
	UBYTE key;                  /* bits 3-0 */
	ULONG information;
	UBYTE additionalLength;     /* the bytes after this one */
	ULONG commandInformation;
	UBYTE asc;
	UBYTE ascq;
	UBYTE fieldReplaceableUnit;
	UBYTE keySpecific[3];
} __attribute__((packed));

/* ---- CDBs ------------------------------------------------------------------ */

/* READ(6) and WRITE(6). */
struct SCSICdb6
{
	UBYTE opcode;
	UBYTE lbaHigh;              /* bits 4-0: LBA bits 20-16 */
	UWORD lbaLow;
	UBYTE length;               /* in blocks; 0 is SCSI_CDB6_BLOCKS_0 */
	UBYTE control;
} __attribute__((packed));

#define SCSI_CDB6_LBA_HIGH              0x1f
#define SCSI_CDB6_BLOCKS_0              256

/* READ, WRITE, WRITE AND VERIFY, VERIFY and WRITE SAME (10). */
struct SCSICdb10
{
	UBYTE opcode;
	UBYTE flags;
	ULONG lba;
	UBYTE group;
	UWORD length;               /* in blocks */
	UBYTE control;
} __attribute__((packed));

/* READ, WRITE, WRITE AND VERIFY and VERIFY (12). */
struct SCSICdb12
{
	UBYTE opcode;
	UBYTE flags;
	ULONG lba;
	ULONG length;               /* in blocks */
	UBYTE group;
	UBYTE control;
} __attribute__((packed));

/* READ, WRITE, WRITE AND VERIFY, VERIFY and WRITE SAME (16). */
struct SCSICdb16
{
	UBYTE opcode;
	UBYTE flags;
	uint64_t lba;
	ULONG length;               /* in blocks */
	UBYTE group;
	UBYTE control;
} __attribute__((packed));

#define SCSI_VERIFY_BYTCHK              0x06    /* in flags: VERIFY compares data */

struct SCSICdbRequestSense
{
	UBYTE opcode;
	UBYTE flags;
	UBYTE reserved[2];
	UBYTE allocationLength;
	UBYTE control;
} __attribute__((packed));

struct SCSICdbInquiry
{
	UBYTE opcode;
	UBYTE flags;
	UBYTE pageCode;
	UWORD allocationLength;
	UBYTE control;
} __attribute__((packed));

#define SCSI_INQUIRY_EVPD               0x01    /* in flags: vital product data */

struct SCSICdbModeSense6
{
	UBYTE opcode;
	UBYTE flags;
	UBYTE page;                 /* page control (bits 7-6) and page code */
	UBYTE subpage;
	UBYTE allocationLength;
	UBYTE control;
} __attribute__((packed));

struct SCSICdbModeSense10
{
	UBYTE opcode;
	UBYTE flags;
	UBYTE page;                 /* page control (bits 7-6) and page code */
	UBYTE subpage;
	UBYTE reserved[3];
	UWORD allocationLength;
	UBYTE control;
} __attribute__((packed));

struct SCSICdbReadCapacity10
{
	UBYTE opcode;
	UBYTE flags;
	ULONG lba;                  /* with PMI only (SBC-2) */
	UBYTE reserved[2];
	UBYTE pmi;                  /* SCSI_READ_CAPACITY_PMI */
	UBYTE control;
} __attribute__((packed));

/* The partial medium indicator: READ CAPACITY(10)'s pmi, READ CAPACITY(16)'s
 * flags. Clear, the LBA must be zero and the answer is the last block; set, it
 * is the last block after the LBA before a substantial delay (SBC-2). */
#define SCSI_READ_CAPACITY_PMI          0x01

/* SERVICE ACTION IN(16), READ CAPACITY(16) among its actions. */
struct SCSICdbServiceAction16
{
	UBYTE opcode;
	UBYTE serviceAction;        /* bits 4-0 */
	uint64_t lba;
	ULONG allocationLength;
	UBYTE flags;
	UBYTE control;
} __attribute__((packed));

#define SCSI_SERVICE_ACTION             0x1f
#define SCSI_READ_CAPACITY_16           0x10    /* the service action */

/* The start of ATA PASS-THROUGH (12) and (16), the same in both. */
struct SCSICdbAtaPassThrough
{
	UBYTE opcode;
	UBYTE protocol;             /* bits 4-1: SCSI_ATA_PROTOCOL() */
	UBYTE flags;
} __attribute__((packed));

#define SCSI_ATA_PROTOCOL(byte)         (((byte) >> 1) & 0x0f)
#define SCSI_ATA_PIO_IN                 4
#define SCSI_ATA_DMA                    6
#define SCSI_ATA_UDMA_IN                10
#define SCSI_ATA_T_DIR                  0x08    /* in flags: data from the device */

/* ---- UNMAP's parameter list ------------------------------------------------ */

struct SCSIUnmapHeader
{
	UWORD dataLength;
	UWORD descriptorsLength;    /* the bytes of descriptors that follow */
	UBYTE reserved[4];
} __attribute__((packed));

struct SCSIUnmapDescriptor
{
	uint64_t lba;
	ULONG blocks;
	UBYTE reserved[4];
} __attribute__((packed));

/* ---- data returned --------------------------------------------------------- */

#define SCSI_TYPE_DIRECT_ACCESS         0x00
#define SCSI_VERSION_SPC3               0x05
#define SCSI_INQUIRY_FORMAT             0x02    /* response data format */

/* Standard INQUIRY data. */
struct SCSIInquiry
{
	UBYTE deviceType;
	UBYTE removable;            /* bit 7: RMB */
	UBYTE version;
	UBYTE responseFormat;
	UBYTE additionalLength;     /* the bytes after this one */
	UBYTE flags[3];
	char vendor[8] __attribute__((nonstring));
	char product[16] __attribute__((nonstring));
	char revision[4] __attribute__((nonstring));
} __attribute__((packed));

/* READ CAPACITY(10) data. */
struct SCSICapacity10
{
	ULONG lastBlock;
	ULONG blockLength;
} __attribute__((packed));

/* READ CAPACITY(16) data. */
struct SCSICapacity16
{
	uint64_t lastBlock;
	ULONG blockLength;
	UBYTE protection;
	UBYTE exponents;
	UWORD lowestAligned;
	UBYTE reserved[16];
} __attribute__((packed));

#define SCSI_MODE_PAGE_CODE             0x3f    /* in a CDB's page byte */
#define SCSI_PAGE_FORMAT                0x03    /* format device */
#define SCSI_PAGE_GEOMETRY              0x04    /* rigid disk geometry */
#define SCSI_PAGE_ALL                   0x3f

/* The mode parameter header of MODE SENSE(6), and of MODE SENSE(10). */
struct SCSIModeHeader6
{
	UBYTE dataLength;           /* the bytes after this field */
	UBYTE mediumType;
	UBYTE deviceSpecific;
	UBYTE descriptorsLength;    /* block descriptors: none here */
} __attribute__((packed));

struct SCSIModeHeader10
{
	UWORD dataLength;           /* the bytes after this field */
	UBYTE mediumType;
	UBYTE deviceSpecific;
	UBYTE reserved[2];
	UWORD descriptorsLength;    /* block descriptors: none here */
} __attribute__((packed));

/* What every mode page starts with. */
struct SCSIPageHeader
{
	UBYTE code;
	UBYTE length;               /* the bytes after this header */
} __attribute__((packed));

/* Mode page 3, format device. */
struct SCSIFormatPage
{
	struct SCSIPageHeader header;
	UWORD tracksPerZone;
	UWORD altSectorsPerZone;
	UWORD altTracksPerZone;
	UWORD altTracksPerUnit;
	UWORD sectorsPerTrack;
	UWORD bytesPerSector;
	UWORD interleave;
	UWORD trackSkew;
	UWORD cylinderSkew;
	UBYTE flags;
	UBYTE reserved[3];
} __attribute__((packed));

/* Mode page 4, rigid disk geometry. */
struct SCSIGeometryPage
{
	struct SCSIPageHeader header;
	UBYTE cylindersHigh;        /* cylinders, bits 23-16 */
	UWORD cylinders;            /* bits 15-0 */
	UBYTE heads;
	UBYTE writePrecompensation[3];
	UBYTE reducedWriteCurrent[3];
	UWORD stepRate;
	UBYTE landingZone[3];
	UBYTE rotationalLock;
	UBYTE rotationalOffset;
	UBYTE reserved1;
	UWORD rotationRate;
	UBYTE reserved2[2];
} __attribute__((packed));

/* The sizes the standards give, checked. */
_Static_assert(sizeof(struct SCSISense) == 18, "fixed-format sense data");
_Static_assert(sizeof(struct SCSICdb6) == 6, "6-byte CDB");
_Static_assert(sizeof(struct SCSICdb10) == 10, "10-byte CDB");
_Static_assert(sizeof(struct SCSICdb12) == 12, "12-byte CDB");
_Static_assert(sizeof(struct SCSICdb16) == 16, "16-byte CDB");
_Static_assert(sizeof(struct SCSICdbModeSense10) == 10, "MODE SENSE(10) CDB");
_Static_assert(sizeof(struct SCSICdbReadCapacity10) == 10, "READ CAPACITY(10) CDB");
_Static_assert(sizeof(struct SCSICdbServiceAction16) == 16, "SERVICE ACTION IN(16) CDB");
_Static_assert(sizeof(struct SCSIUnmapDescriptor) == 16, "UNMAP block descriptor");
_Static_assert(sizeof(struct SCSIInquiry) == 36, "standard INQUIRY data");
_Static_assert(sizeof(struct SCSICapacity16) == 32, "READ CAPACITY(16) data");
_Static_assert(sizeof(struct SCSIModeHeader10) == 8, "MODE SENSE(10) header");
_Static_assert(sizeof(struct SCSIFormatPage) == 24, "format device page");
_Static_assert(sizeof(struct SCSIGeometryPage) == 24, "rigid disk geometry page");

#endif
