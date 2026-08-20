
// mounter_internal.h — types and helpers shared by the mounter's four sources.
//
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
//  - printf(): important output (errors/warnings/partition dumps). Routed to
//    the host-provided mounter_log() sink when MOUNTER_LOG is defined by the
//    build; otherwise compiles away entirely.
//  - dbg(): verbose per-step tracing, layered on top of printf(). Define
//    MOUNTER_TRACE=1 from the build to enable it; off by default. Still
//    compiles away if MOUNTER_LOG is undefined, since dbg() -> printf().
#if defined(MOUNTER_LOG)
// Host-provided log sink (e.g. the Poseidon debug backend). It may format with
// exec RawDoFmt, so format strings here use %l-sized conversions only (no %p).
void mounter_log(const char *fmt, ...);
#define printf mounter_log
#else
#define printf(...)
#endif

#ifndef MOUNTER_TRACE
#define MOUNTER_TRACE 0
#endif

#if MOUNTER_TRACE
#define dbg printf
#else
/* Swallow the whole call: a bare `#define dbg` leaves the argument list behind
 * as a comma expression, which every caller then warns about. */
#define dbg(x...) do { } while (0)
#endif

#define MAX_BLOCKSIZE 4096
// Scratch sectors, see sector_take(). Deepest nest is the GPT path: block 0, the
// GPT header, the partition-entry array, and one partition's VBR.
#define SECTOR_SLOTS 4

struct MountData
{
	struct ExecBase *SysBase;
	struct ExpansionBase *ExpansionBase;
	struct DosLibrary *DOSBase;
	struct IOExtTD *request;
	struct ConfigDev *configDev;
	const UBYTE *creator;
	const UBYTE *devicename;

	UBYTE *sector[SECTOR_SLOTS];    /* scratch sector pool, see sector_take() */
	UWORD sectorsUsed;

	ULONG unitnum;
	LONG mounted;                   /* volumes mounted on the current unit */
	BOOL wasLastDev;

	ULONG blocksize;
	ULONG totalsectors;

	ULONG flags;                    /* MSF_* */
	const struct MountFS *fs[MOUNTFS_KINDS];   /* indexed by MOUNTFS_* */
	ULONG dmaAlign;                 /* requested buffer alignment in bytes, 0 = default */
	BOOL legacyMounted;             /* per unit, for MSF_LEGACY_FIRST_ONLY */
	LONG deferred;                  /* volumes skipped for want of a loadable handler */
	LONG alreadyMounted;            /* volumes left alone because this extent is mounted */
	LONG renamed;                   /* DOS names bumped past a collision */
};

struct ParameterPacket
{
	const UBYTE *dosname;
	const UBYTE *execname;
	ULONG unitnum;
	ULONG flags;
	struct DosEnvec de;
};

// One mountable volume, as found by a scanner and handed to mount_volume().
//
// The scanners differ only in how they fill this in. The RDB scanner copies the
// DosEnvec straight off the disk and pre-resolves the filesystem from the RDB's
// FSHD chain; the CD and MBR/GPT scanners synthesise the DosEnvec from a block
// extent (envec_from_recipe) and name a MountFS recipe for mount_volume to
// resolve. At most one of fs/fse is set — both NULL is an RDB partition whose
// dostype nothing claims, which still mounts on the ROM filesystem.
// What became of one volume. The scanners no longer each invent their own
// -1/0/count encoding: mount_volume() says what happened, and keeps md->mounted
// and the md->already/renamed counters in step with it, so the numbers the caller
// finally sees cannot drift from what the code actually did.
enum MountOutcome
{
	MOUNT_OK,        /* node created, filesystem attached, node added */
	MOUNT_ALREADY,   /* this device/unit/extent is already mounted; left alone */
	MOUNT_FAILED,    /* could not create the node, or no filesystem for it */
};

// bn_Node.ln_Pri for a node that must never be a boot candidate. expansion.doc:
// "-128 -- don't even bother to boot from this device".
#define MOUNT_NEVER_BOOT  (-128)

struct Volume
{
	struct ParameterPacket pp;    /* MakeDosNode() argument; de filled by the scanner */
	const UBYTE *nameHint;        /* C string: RDB pb_DriveName, or the recipe's dosName */
	LONG bootPri;                 /* passed to AddBootNode(); -128 = never boot */
	const struct MountFS *fs;     /* recipe path; NULL on the RDB path */
	struct FileSysEntry *fse;     /* RDB path: resolved from the FSHD chain; NULL otherwise */
};


/* ---- mounter.c: the pieces every scanner uses ------------------------------ */

BOOL   mnt_read_block(UBYTE *buf, ULONG block, ULONG id, struct MountData *md);
UBYTE *mnt_sector_take(struct MountData *md);
void   mnt_sector_drop(struct MountData *md, const UBYTE *buf);
struct FileSysEntry *mnt_find_filesystem(ULONG id1, ULONG id2, struct ExecBase *SysBase);
BOOL   mnt_resolve_fs(struct MountData *md, const struct MountFS *fs, ULONG lowCyl);
void   mnt_envec_from_recipe(struct MountData *md, struct DosEnvec *de,
                             const struct MountFS *fs, ULONG lowCyl, ULONG highCyl,
                             LONG bootPri);
enum MountOutcome mnt_mount_volume(struct MountData *md, struct Volume *vol);

/* ---- the three scanners ---------------------------------------------------- */
/* Each answers one question — did you recognize this medium? — and leaves the
   number of volumes it mounted in md->mounted. */

BOOL mnt_scan_rdb(struct MountData *md);      /* mounter_rdb.c    */
BOOL mnt_scan_legacy(struct MountData *md);   /* mounter_legacy.c */
BOOL mnt_scan_cd(struct MountData *md);       /* mounter_cd.c     */

#endif /* MOUNTER_INTERNAL_H */
