# Mounter

A generic autoboot and automount mounter for AmigaOS device drivers. Given a
device and its units, it reads each medium's layout (RDB, MBR or GPT, a
filesystem with no partition table, or a CD), finds or loads the filesystems,
and adds a DOS device for every volume it can mount. It works the same from a
Kickstart-resident driver before DOS exists, where the devices it adds are
what the system boots from, and from a hotplug driver on a running system.

This is the `neptune-fixes` fork of the a4091-software mounter by Toni Wilen,
Stefan Reinauer and Matt Harlum. It serves the Neptune USB stack's
`massstorage.class` (`usbscsi.device`) and the Emu68 driver stack's
`nvme.device`, and no longer tracks upstream. [CHANGELOG.md](CHANGELOG.md) has
its history, [ARCHITECTURE.md](ARCHITECTURE.md) how it works inside.

## 1. What it mounts

| Medium | What happens |
|---|---|
| **RDB** in the first 16 blocks | Every partition, with the RDB's own geometry, name and boot flags. A filesystem stored on the disk (FSHD and LSEG blocks) is loaded when `FileSystem.resource` has no entry for its dostype at that version or newer. `PBFF_NOMOUNT` partitions are skipped; `RDBFF_LAST` ends the unit list (see `MSF_IGNORE_LAST`). |
| **MBR**, with extended partitions (types `0x05`, `0x0F`, `0x85`) | Every partition whose own boot sector is FAT, NTFS or exFAT, through the caller's recipe for that family. The type byte is only a hint: sticks are often mislabelled, and `0x07` is NTFS or exFAT. |
| **GPT** | Microsoft Basic Data partitions, sniffed the same way. The GPT is believed only behind a protective (or hybrid) MBR entry and with a valid header, so a stale header left by earlier partitioning cannot override the current MBR. |
| **Superfloppy** | A FAT, NTFS or exFAT filesystem at block 0, with no partition table, mounts as one volume covering the medium. |
| **CD, DVD, BD** | One volume for the whole disc. By default only ISO 9660 data discs; a recipe whose handler reads other formats or audio discs says so with `MOUNTFS_CD_ANYFMT` and `MOUNTFS_CD_AUDIO`. A data disc without an ISO 9660 descriptor is tried as an RDB CD first. |
| **Virtual drive**: MBR type `0x76` or Emu68's GPT type `3F82EEBC-87C9-4097-8165-89D6540557C0` | A whole RDB disk inside one partition, as on Emu68's SD card. Never mounted on the unit that holds it; the host's virtual device (section 5) makes it a unit of its own, where its RDB mounts and boots. |

Rules that hold for every medium:

- **Booting.** Only RDB partitions marked bootable, and Amiga-bootable CDs (an
  ISO 9660 system ID of `AMIGA BOOT` or `CDTV`, at priority 5), can boot. MBR,
  GPT and superfloppy volumes never can: a FAT, NTFS or exFAT boot sector sits
  where an Amiga boot block would. Boot nodes are made only before DOS exists;
  `MSF_NO_BOOT` turns them off.
- **DOS names.** An RDB partition keeps its own name. A recipe name gets a unit
  digit (`UMSD` becomes `UMSD0`), since every volume of that recipe asks for
  it. A name already in use, before or after DOS starts, is bumped (`UMSD1`, …,
  `UMSD10`).
- **No volume twice.** A volume whose device, unit, geometry and blocks match a
  DOS device that already exists is left alone. Calling `MountDrive()` again
  after DOS has started therefore mounts only what could not be mounted the
  first time.
- **Untrusted media.** Every on-disk field is checked before it is used:
  ARCHITECTURE.md section 8 lists how.

## 2. Requirements

- AmigaOS 3.1 (V40) or later; any 680x0, the 68000 included.
- `exec.library` and `expansion.library` V40. `dos.library` V40 is optional:
  the mounter opens it if it can, and its absence is how it knows it runs
  before DOS exists.
- A task or process to call it from: it does its own synchronous I/O on the
  units. From a task rather than a process, a recipe's handler file cannot be
  checked for existence and is taken on trust.

## 3. Building it into a driver

Include `mounter.cmake` from the host's `CMakeLists.txt`:

```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/mounter/mounter.cmake)
add_executable(mydriver.device ... ${MOUNTER_SOURCES})
```

`MOUNTER_SOURCES` is every source, `bootpoint.S` included;
`MOUNTER_C_SOURCES` is the C files alone, for per-file compile options. A host
that serves virtual drives (section 5) adds `VDRIVE_SOURCES` as well, all C
files. Add the mounter's directory to the include path and include
`mounter.h`.

The mounter keeps no writable globals, so it links into a ROM module as it is.
It builds warning-free with `-Wall -Wextra -Wconversion -Wsign-conversion
-Wshadow -Wmissing-prototypes -Wstrict-prototypes`.

**Diagnostics.** Define `MOUNTER_LOG` and provide

```c
void mounter_log(const char *fmt, ...);
```

to receive the mounter's messages: errors, warnings and each unit's partition
table. The format strings use `%l`-sized conversions only, so a sink built on
exec's `RawDoFmt()` formats them correctly. Add `MOUNTER_TRACE=1` for a trace
of every step. Without `MOUNTER_LOG`, all of it compiles away.

## 4. Calling `MountDrive()`

```c
LONG MountDrive(const struct MountStruct *ms, struct MountResult *res);
```

Zero-fill a `struct MountStruct`, set what you need and pass it with an
optional `struct MountResult`. The mounter never writes to the `MountStruct`,
except through `unitResults`. `mounter.h` documents every field; in short:

| `MountStruct` field | Meaning |
|---|---|
| `deviceName` | the exec device to open, e.g. `"usbscsi.device"` |
| `units`, `unitCount` | the unit numbers to probe, in order; both required |
| `unitResults` | optional, `unitCount` entries: per unit -1 nothing recognized (or the unit would not open), 0 recognized but nothing mounted, *n* volumes mounted, -2 skipped after an earlier unit's `RDBFF_LAST` |
| `creatorName` | `ln_Name` of the `FileSystem.resource` entries loaded off a disk; NULL: the device name |
| `configDev` | an autoconfig board's `ConfigDev`, for booting from it; NULL: one is made when the first bootable volume is mounted before DOS exists |
| `SysBase` | exec |
| `flags` | `MSF_*`, below |
| `dmaAlign` | the device's preferred buffer alignment; nonzero puts the recipe-mounted filesystems' buffers in Fast RAM. The alignment itself is not imposed on them (no filesystem honours `de_Mask` well), so the device must bounce misaligned buffers |
| `fs[]` | recipes, indexed by `MOUNTFS_FAT`, `MOUNTFS_NTFS`, `MOUNTFS_EXFAT`, `MOUNTFS_CD` |

| Flag | Effect |
|---|---|
| `MSF_NO_RDB` | skip the RDB scan |
| `MSF_NO_LEGACY` | skip MBR, GPT and superfloppy volumes |
| `MSF_NO_CD` | skip CD units, RDB CDs included |
| `MSF_LEGACY_FIRST_ONLY` | mount only the first MBR, GPT or superfloppy volume per unit |
| `MSF_NO_BOOT` | never make a boot node |
| `MSF_SLOW_SPINUP` | 15 read attempts instead of 3, for drives slow to spin up |
| `MSF_IGNORE_LAST` | keep probing units after an RDB with `RDBFF_LAST` |

**Recipes.** A `struct MountFS` tells the mounter how to mount one filesystem
family found outside an RDB: its dostype, an optional handler file
(`"L:fat95"`), the preferred DOS name, `de_Control`, buffers, MaxTransfer, the
handler's stack and `MOUNTFS_*` flags. A NULL recipe means that family is not
mounted, except FAT and CD, which fall back to the classic dostypes (FAT95;
CD01 or CDVD) from `FileSystem.resource`.

A recipe's filesystem is the `FileSystem.resource` entry for its dostype if
there is one, else its handler file, which DOS loads on first access. Flags
describe what the handler can do:

| `MOUNTFS_*` flag | Effect |
|---|---|
| `MOUNTFS_FORCELOAD` | prefer the handler file over a resource entry on the same dostype, as `ForceLoad=1` does in a mountlist: for dostypes a controller ROM has likely taken (`CD01`). The resource entry stays the fallback when the handler file is missing |
| `MOUNTFS_CD_ANYFMT` | the CD handler identifies disc formats itself (High Sierra, UDF, HFS); it is handed any data disc, and a disc whose table of contents cannot be read |
| `MOUNTFS_CD_AUDIO` | the CD handler can present audio discs; they mount instead of being refused |

A recipe that resolves to nothing mounts nothing. Before DOS exists a handler
file cannot be loaded, so such a volume is *deferred*: counted, and left for a
call made once DOS is up.

**Result.** `MountDrive()` returns the number of volumes on the units (> 0),
0 if a medium was recognized but nothing mounted, or -1 if nothing was
recognized or the call could not start. `MountResult` breaks that down:

| Field | Meaning |
|---|---|
| `mounted` | volumes on the units after the call: mounted now or found already mounted |
| `deferred` | volumes waiting for DOS to load their handler file; nonzero is the reason to call again once DOS is up |
| `alreadyMounted` | volumes found already mounted and left alone |
| `renamed` | volumes whose DOS name had to be bumped. Normal for two drives that want the same name; on one drive, the sign of a volume mounted twice |
| `recognized` | some unit carried a medium the mounter knows |

**Pre-DOS and post-DOS.** A boot ROM calls `MountDrive()` before DOS exists:
what it mounts then can boot. If `res->deferred` is nonzero, call again once
DOS is up; volumes already mounted are left alone, the deferred ones mount. A
hotplug driver simply calls it whenever a medium arrives.

## 5. Virtual drives (`vdrive.h`)

A virtual drive is a partition holding a whole RDB disk: the RDB sits in its
first blocks, and every block number inside counts from the partition start.
The host serves each one as a unit of a separate exec device, addressed from
its first block, so the same volumes read alike here and on Emu68.
`vdrive.h` documents the calls.

**One device per host.** The host names it (`nvmevd.device`,
`usbscsivd.device`) and keeps a `struct VDrive *`, NULL until
`VDriveRefresh()` finds the first virtual drive and creates the device. A
system without virtual drives never gets one, and every other call accepts
NULL.

**A handle per parent unit.** With each of its own units the host also keeps a
`struct VDriveParent *`, which `VDriveRefresh()` sets once that unit has had a
virtual drive, and passes it to the calls about the unit. NULL means the unit
has none, and those calls then return at once. A handle stays valid until
`VDriveDestroy()`.

**Units.** `VDRIVE_UNIT(parent, slot)` = `parent * 10 + slot`, the slot being
the virtual drive's order on its parent unit. HDToolBox reads a unit number as
address + 10 × LUN, so it shows the slot as the address and the parent unit as
the LUN. Ten slots per parent are served, and a unit is at most 2^32 blocks; a
virtual drive past them gets no unit and stays an ordinary partition of the
parent.

**Write protection on the parent.** On the parent unit, the blocks of every
virtual drive that has a unit are read-only to everyone but the virtual
device, so that no two paths write the same blocks. A virtual drive without a
unit is reachable through the parent alone, and writable there. The host asks
on its write paths:

- `VDriveWriteAllowed(parent, io, offset, length)` for every write, format,
  write-zeroes or trim, by byte range;
- `VDriveScsiAllowed(parent, io, scsi)` for `HD_SCSICMD`: reads and queries
  pass, WRITE, WRITE AND VERIFY and WRITE SAME are checked by range, UNMAP by
  descriptor, ATA PASS-THROUGH passes only for data-in, and anything else is
  refused with DATA PROTECT (07/27h) in the sense data.

A refused write completes with `TDERR_WriteProt`, a refused SCSI command with
`HFERR_BadStatus`.

**Host duties.**

- `VDriveRefresh(&device, config, parentUnit, &parent)` when a unit's medium is
  new, from a task that is not the parent's I/O task (it reads the partition
  table). It sets the unit's handle and returns how many slots are online;
  mount those units with `MSF_NO_LEGACY | MSF_NO_CD | MSF_IGNORE_LAST`.
- `VDriveParentGone(parent)` when the medium or the unit goes: no I/O, safe
  from any task.
- `VDriveComplete(device, io)` on every path that replies a finished request,
  from a task, never an interrupt, and `ReplyMsg()` only when it returns FALSE.
  The virtual device's own requests come back no other way: one that is
  `ReplyMsg()`ed is never completed, and its client waits for good.
- `VDriveAnyFenced(device)` before a command that wipes the whole device, which
  no unit's check can cover (an NVMe Format or Sanitize): refuse it while this
  is TRUE.
- `VDriveDestroy(device)` before expunging, and only when the host's open count
  is down to `VDriveHeldOpens(device)` (plus its own): the device keeps every
  parent unit that had a virtual drive open. It refuses while one of its own
  units is open.

`VDriveRefresh()` and `VDriveDestroy()` do I/O and change the device: make
them from one task at a time.

How the device works inside is in ARCHITECTURE.md section 9.

## 6. License

Copyright 2021-2022 Toni Wilen

The MBR/GPT on-disk structures (`legacy.h`) are Copyright 2022-2023
Stefan Reinauer & Chris Hooper (BSD-2-Clause, from a4091-software).

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
