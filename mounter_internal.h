// SPDX-License-Identifier: BSD-2-Clause
// mounter_internal.h — what the mounter's units share: the state of one
// MountDrive() call, the volume and partition-table types, and one section of
// prototypes per unit. The sections run bottom-up: a unit calls only into the
// sections above its own, and the scanners last of all.
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
// The full licence, its disclaimer included, is in LICENSE.
//
#ifndef MOUNTER_INTERNAL_H
#define MOUNTER_INTERNAL_H

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

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/dos.h>

#include "mounter.h"

#ifndef SID_TYPE
#define SID_TYPE 0x1F
#endif

// Two independent, host-configured logging tiers:
//  - mnt_log(): important output (errors/warnings/partition dumps). Routed to
//    the host-provided mounter_log() sink when MOUNTER_LOG is defined by the
//    build; otherwise compiles away entirely.
//  - dbg(): verbose per-step tracing, layered on top of mnt_log(). Define
//    MOUNTER_TRACE=1 from the build to enable it; off by default. Still
//    compiles away if MOUNTER_LOG is undefined, since dbg() -> mnt_log().
#if defined(MOUNTER_LOG)
// Host-provided log sink (e.g. the Neptune debug backend). It may format with
// exec RawDoFmt, so format strings here use %l-sized conversions only (no %p).
void mounter_log(const char *fmt, ...);
#define mnt_log mounter_log
#else
#define mnt_log(...)
#endif

#ifndef MOUNTER_TRACE
#define MOUNTER_TRACE 0
#endif

#if MOUNTER_TRACE
#define dbg mnt_log
#else
/* Swallow the whole call: a bare `#define dbg` leaves the argument list behind
 * as a comma expression, which every caller then warns about. */
#define dbg(x...) do { } while (0)
#endif

#define MAX_BLOCKSIZE 4096
// Scratch sectors, see mnt_sector_take(). The deepest nests are three: the RDB
// scan (RDSK/PART, FSHD, LSEG) and the partition table (block 0, GPT header,
// entry array).
#define SECTOR_SLOTS 3

// No block: the end of an RDB block chain (rdb_PartitionList, pb_Next,
// rdb_FileSysHeaderList, fhb_Next, lsb_Next).
#define RDB_END 0xffffffff

// bn_Node.ln_Pri for a node that must never be a boot candidate. expansion.doc:
// "-128 -- don't even bother to boot from this device".
#define MOUNT_NEVER_BOOT  (-128)

/* ---- the partition table (mounter_table.c) --------------------------------- */

// What a partition-table entry holds, as far as the mounter is concerned.
enum PartKind
{
	PART_OTHER,      /* not ours: a GPT type we do not mount, a GPT protective entry */
	PART_DATA,       /* may hold FAT, NTFS or exFAT; its boot sector decides */
	PART_VIRTUAL,    /* a virtual drive: a whole RDB disk, the virtual device's */
};

// How block 0 is laid out.
enum PartScheme
{
	TABLE_NONE,          /* no table: an RDB, a blank or unreadable medium, a refused GPT */
	TABLE_MBR,           /* an MBR, with its EBR chains */
	TABLE_GPT,           /* a GPT */
	TABLE_SUPERFLOPPY,   /* a filesystem at block 0: one PART_DATA entry, the whole medium */
};

// GPT's standard entry count, and more than an MBR's four slots and their EBR
// chains can reach.
#define TABLE_MAX 128

// One partition, as the table names it.
struct Partition
{
	uint64_t start;             /* first block */
	uint64_t length;            /* in blocks, at least 1 */
	enum PartKind kind;
	UBYTE mbrType;              /* MBR/EBR type byte, for the log; 0 for GPT */
	UBYTE number;               /* for the log: 1-4 primary, 5 on logical, GPT entry + 1 */
};

// A unit's partition table, in table order.
struct PartTable
{
	enum PartScheme scheme;
	UWORD count;                /* entries in part[] */
	struct Partition part[TABLE_MAX];
};

/* ---- the state of one MountDrive() call ------------------------------------ */

struct MountData
{
	/* The call: set by MountDrive(), constant while it runs. */
	struct ExecBase *SysBase;
	struct ExpansionBase *ExpansionBase;
	struct DosLibrary *DOSBase;     /* NULL before DOS exists, which selects the pre-DOS paths */
	const UBYTE *devicename;
	const UBYTE *creator;           /* ln_Name of the FileSystem.resource entries added */
	ULONG flags;                    /* MSF_* */
	ULONG dmaAlign;                 /* device DMA alignment, nonzero = FS buffers in Fast RAM */
	const struct MountFS *fs[MOUNTFS_KINDS];   /* indexed by MOUNTFS_*; FAT and CD never NULL */
	struct ConfigDev *configDev;    /* the caller's, or made on the first bootable volume */

	/* The unit being scanned: set by probe_unit(). */
	struct IOExtTD *request;        /* open on the unit */
	ULONG unitnum;
	ULONG blocksize;
	ULONG totalsectors;
	LONG mounted;                   /* volumes mounted on it, or found already mounted */
	BOOL wasLastDev;                /* its RDB set RDBFF_LAST */
	struct PartTable table;         /* direct access: its partition table */

	/* Scratch sectors, see mnt_sector_take(). */
	UBYTE *sector[SECTOR_SLOTS];
	UWORD sectorsUsed;

	/* Totals over every unit, for MountResult. */
	LONG total;                     /* volumes on recognized units */
	BOOL recognized;                /* some unit's medium was recognized */
	LONG deferred;                  /* volumes skipped for want of a loadable handler */
	LONG alreadyMounted;            /* volumes left alone because this extent is mounted */
	LONG renamed;                   /* DOS names bumped past a collision */
};

/* ---- volumes ---------------------------------------------------------------- */

struct ParameterPacket
{
	const UBYTE *dosname;
	const UBYTE *execname;
	ULONG unitnum;
	ULONG flags;
	struct DosEnvec de;
};

// What became of one volume. mnt_mount_volume() says what happened, and keeps
// md->mounted and the md->deferred/alreadyMounted/renamed counters in step with
// it, so the numbers the caller finally sees cannot drift from what the code
// actually did.
enum MountOutcome
{
	MOUNT_OK,        /* node created, filesystem attached, node added */
	MOUNT_ALREADY,   /* this device/unit/extent is already mounted; left alone */
	MOUNT_DEFERRED,  /* the filesystem is a handler file, and there is no DOS yet to load it */
	MOUNT_FAILED,    /* could not create the node, or no filesystem for it */
};

// One mountable volume, as found by a scanner and handed to mnt_mount_volume().
//
// The RDB scanner copies the DosEnvec straight off the disk and resolves the
// filesystem from the RDB's FSHD chain. The CD and MBR/GPT scanners name a
// MountFS recipe and the volume's blocks instead; mnt_mount_volume() builds
// their DosEnvec and resolves the recipe.
struct Volume
{
	struct ParameterPacket pp;    /* MakeDosNode() argument; RDB: de as on the disk */
	const UBYTE *nameHint;        /* C string: RDB pb_DriveName, or the recipe's dosName */
	LONG bootPri;                 /* passed to AddBootNode(); -128 = never boot */
	struct FileSysEntry *fse;     /* RDB: from the FSHD chain or FileSystem.resource;
	                                 NULL: the ROM filesystem */
	const struct MountFS *fs;     /* recipe; NULL on the RDB path */
	ULONG firstBlock;             /* recipe: the volume's first and last block; */
	ULONG lastBlock;              /*   0 and 0 for a whole medium, such as a CD */
};

// What a volume's DeviceNode will run, and the strings it takes over once added.
struct FsChoice
{
	struct FileSysEntry *fse;     /* patched in by its PatchFlags, unless handler is set */
	UBYTE *handler;               /* BSTR: a handler file DOS loads on first access */
	ULONG stackSize;              /* dn_StackSize with handler */
	UBYTE *control;               /* BSTR: the recipe's de_Control; NULL: none */
};

/* ---- mounter_io.c: reading the unit being scanned ---------------------------- */
/* Every function's contract is its kernel-doc at the definition. */

BOOL   mnt_read_block(UBYTE *buf, uint64_t block, struct MountData *md);
BOOL   mnt_read_rdb_block(UBYTE *buf, ULONG block, ULONG id, struct MountData *md);
UBYTE *mnt_sector_take(struct MountData *md);
void   mnt_sector_drop(struct MountData *md, const UBYTE *buf);
void   mnt_sector_pool_free(struct MountData *md);

/* ---- mounter_fs.c: filesystems ---------------------------------------------- */

struct FileSysEntry *mnt_find_filesystem(ULONG id1, ULONG id2, struct ExecBase *SysBase);
struct FileSysEntry *mnt_fse_new(struct MountData *md, const struct FileSysHeaderBlock *fshb);
BOOL   mnt_fse_register(struct MountData *md, struct FileSysEntry *fse);
enum MountOutcome mnt_resolve_fs(struct MountData *md, const struct MountFS *fs,
                                 struct FsChoice *fc);
void   mnt_attach_fs(struct DeviceNode *dn, const struct FsChoice *fc);
void   mnt_release_fs(struct MountData *md, struct FsChoice *fc);

/* ---- mounter_volume.c: volumes into DOS devices ------------------------------ */

enum MountOutcome mnt_mount_volume(struct MountData *md, struct Volume *vol);

/* ---- mounter_table.c: the partition table ------------------------------------ */

BOOL   mnt_vbr_kind(const UBYTE *b, ULONG blocksize, enum MountFSKind *kind);
BOOL   mnt_read_table(struct MountData *md);
BOOL   mnt_in_virtual_drive(const struct PartTable *pt, ULONG block);

/* ---- mounter_hunk.c: filesystems loaded off the disk ------------------------- */

BPTR   mnt_load_lseg(struct MountData *md, ULONG block, UBYTE *buf);
void   mnt_unload_lseg(struct MountData *md, BPTR seglist);

/* ---- the three scanners ------------------------------------------------------ */
/* Each answers one question — did you recognize this medium? — and leaves the
   number of volumes it mounted in md->mounted. */

BOOL mnt_scan_rdb(struct MountData *md);      /* mounter_rdb.c    */
BOOL mnt_scan_legacy(struct MountData *md);   /* mounter_legacy.c */
BOOL mnt_scan_cd(struct MountData *md);       /* mounter_cd.c     */

/* ---- mounter.c: the virtual-drive device's way in ---------------------------- */
/* A virtual drive is an MBR partition of type 0x76 (primary or logical) or a GPT
   partition of Emu68's type 3F82EEBC-87C9-4097-8165-89D6540557C0 holding a whole
   RDB disk. The mount pass leaves them to the host's virtual device (vdrive.c),
   which lists them with this. */

LONG mnt_find_virtual_drives(struct ExecBase *SysBase, const UBYTE *deviceName, ULONG unitNum,
                             struct DriveGeometry *geom, struct Partition *out, ULONG max);

#endif /* MOUNTER_INTERNAL_H */
