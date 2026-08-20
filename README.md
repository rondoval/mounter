# Mounter: AmigaOS Generic Mounter

Generic autoboot / automount partition parser and mounter: RDB, MBR, GPT,
superfloppy and data CDs.

This is the mounter code from `a4091.device` (`poseidon-fixes` fork; see
CHANGELOG.md for the differences against upstream).

## 1. Introduction

This Mounter software is a generic, autoboot/automount Rigid Disk Block (RDB)
parser and mounter for AmigaOS. Originally developed by Toni Wilen and extended
by Stefan Reinauer and Matt Harlum, it is used by the `a4091.device` and
`lide.device` drivers; this fork additionally drives the Poseidon USB stack's
`massstorage.class` (hotplugged media). It is engineered to be a robust and
highly compatible solution for device drivers that need to mount partitions
and filesystems — at boot time or at hotplug time.

It is designed to be highly portable and 68000-compatible, and to work equally
well from a Kickstart-resident driver (before DOS exists, where the nodes it
adds are what `strap` boots from) and from a hotplug driver on a running
system. Its primary function is to scan storage devices, interpret
their partition maps (RDB, MBR, GPT, or none), load or locate the necessary
filesystems, and make the partitions available to the operating system.

---

## 2. Core Features

* **OS Requirement**: AmigaOS 3.1 (V40) and up. The `poseidon-fixes` fork
  dropped upstream's Kickstart 1.3/2.x fallbacks and uses the V36+ OS API
  unconditionally.
* **CPU Compatibility**: Compatible with the Motorola 68000 processor, ensuring
  it runs on all classic Amiga models.
* **Autoboot Capability**: Can participate in the Amiga's autoconfig boot
  process to mount bootable partitions.
* **Full Automount Support**: Automatically finds and mounts all valid
  partitions it discovers.
* **Rich Partition/Filesystem Support**:
    * **RDB**: Full support for the Amiga Rigid Disk Block standard, including
      loading and relocating filesystems embedded on disk (FSHD/LSEG).
    * **MBR & GPT**: PC-style Master Boot Records (including 0x05/0x0F/0x85
      extended-partition chains) and GUID Partition Tables (validated header,
      gated by the protective MBR entry).
    * **Superfloppy**: a filesystem at block 0 with no partition table mounts
      as a whole-disk device.
    * **Content sniffing**: each legacy partition's boot sector is inspected
      (`DetectVBR`) — FAT, NTFS and exFAT are told apart, each mounting through
      its own recipe; the MBR type byte / GPT type GUID is treated only as a
      hint (type `0x07` is NTFS *and* exFAT). Unsupported content is skipped
      instead of mounted wrongly.
    * **CD-ROM**: data discs mount as read-only whole-medium volumes and
      Amiga-bootable CDs ("AMIGA BOOT" / "CDTV" system ID) get boot priority;
      RDB-formatted CDs are also supported. Which *formats* are accepted is the
      CD recipe's call: by default the disc must be ISO 9660, but a handler
      that identifies formats itself (`MSF_CD_ANYFMT`) is handed any data disc,
      and one that understands audio tracks (`MSF_CD_AUDIO`) is handed
      audio-only discs too.
* **Filesystem recipes**: the caller controls, per filesystem family
  (FAT/NTFS/exFAT/CD), the dostype, an optional handler file loaded by DOS on
  first access (no FileSystem.resource entry needed), the preferred DOS device
  name, `de_Control`, buffers, MaxTransfer, stack size and `MOUNTFS_*` flags.
  See `struct MountFS`. A recipe whose dostype is unregistered *and* whose
  handler file cannot be found is skipped rather than mounted into a node that
  fails on first access (checked only post-DOS, from a Process).
  `MOUNTFS_FORCELOAD` inverts the usual resolution order so the recipe's
  handler file wins over a `FileSystem.resource` entry claiming the same
  dostype — the mountlist `ForceLoad = 1`, for dostypes a controller ROM is
  likely to have taken already.
* **Explicit unit mounting**: the caller names the unit(s) to mount — a single
  unit or a list — with per-unit results reported back. (The `poseidon-fixes`
  fork dropped upstream's blind SCSI target/LUN scan; its consumers are not
  SCSI hosts and always know their units.)
* **Collision-safe device naming**: preferred names get a trailing digit
  ensured ("UMSD" → "UMSD0") and bumped past collisions ("UMSD1" … "UMSD10"),
  checked against both the pre-boot MountList and the live DOS lists.
* **Hardened against corrupt media**: untrusted on-disk fields are clamped,
  block chains are cycle-capped, and the hunk relocator is overflow-checked
  (see 4.4).

---

## 3. High-Level Architecture

The Mounter's architecture is centered around a single primary entry point,
`MountDrive()`, which orchestrates the entire mounting process. It operates
by opening a specified device driver, scanning its units, and attempting to
identify and mount partitions.

### 3.1. Key Data Structures

* **`struct MountStruct` (`mounter.h`)**: This is the main input structure
  passed to the `MountDrive` function. It defines the parameters for a mounting
  session.
    * `deviceName`: The name of the device driver to use (e.g., `scsi.device`).
    * `unitNum`: required. A value < 0x100 = that single unit; otherwise a
      pointer to a `{count, unit0, unit1, ...}` array. Array entries are
      overwritten with each unit's result.
    * `creatorName`: A string to identify the creator of the filesystem entries.
    * `configDev`: A pointer to the `ConfigDev` structure for an autoconfig
      board, essential for autobooting.
    * `slowSpinup`, `ignoreLast`: Boolean flags controlling spin-up delays and
      the handling of the RDB `RDBFF_LAST` flag.
    * `flags`: `MSF_*` flags gating the RDB / MBR-GPT-superfloppy / CD scans
      and boot-node creation.
    * `fatFS`, `ntfsFS`, `cdFS`: filesystem recipes (see below); NULL keeps
      the classic behavior.
    * `SysBase`: A pointer to the Exec library base.

* **`struct MountFS` (`mounter.h`)**: A filesystem recipe for non-RDB media:
  dostype, optional handler file, preferred DOS device name, `de_Control`
  string, buffers, MaxTransfer and handler stack size. All strings are owned
  by the caller for the duration of `MountDrive()`.

* **`struct MountData` (`mounter.c`)**: An internal state-management structure
  used during the `MountDrive` execution. It holds pointers to opened
  libraries, the I/O request, device geometry, and state variables for the
  scanning process.

### 3.2. Control Flow

The logical flow of the `MountDrive` function is as follows:

1.  **Initialization**:
    * Allocate a `MountData` structure to hold operational state.
    * Open required system libraries (`expansion.library`, `dos.library`).
    * Create a message port and an I/O request for communicating with the
      device driver.

2.  **Unit Selection**:
    * Probe exactly the given unit(s) (`ScanUnitList`), writing each unit's
      result back into the caller's array and honoring `RDBFF_LAST` unless
      `ignoreLast` is set.
    * Each unit is probed by `ProbeUnit()`: `OpenDevice()`, geometry, scan,
      motor off, `CloseDevice()`.

3.  **Partition Scheme Identification** (per unit):
    * Get the geometry via `TD_GETGEOMETRY`; sector sizes 256–4096 are
      accepted.
    * **Direct-access devices**: try RDB first — scan the first
      `RDB_LOCATION_LIMIT` blocks (`ScanRDSK`, unless `MSF_NO_RDB`). If no RDB,
      classify block 0 (`ScanLegacy`, unless `MSF_NO_LEGACY`): a GPT (gated by
      its protective MBR entry and a validated header at block 1), else a
      filesystem VBR at block 0 (superfloppy), else a sane MBR.
    * **CD/WORM/optical devices** (unless `MSF_NO_CD`): check unit ready, then
      classify the disc from its TOC (`ClassifyCD`: data track 1, audio track 1,
      or unreadable). For a data disc, read the ISO PVD (`CheckPVD`) — used for
      "AMIGA BOOT"/"CDTV" boot priority only. No PVD → RDB-CD fallback, then
      mount anyway if the recipe declares `MSF_CD_ANYFMT` (an unreadable
      sector 16 is still refused). An unreadable TOC is likewise treated as a
      data disc under `MSF_CD_ANYFMT`, since some enclosures answer READ TOC
      poorly for DVD/BD media.

4.  **Partition Processing**:
    * The appropriate parser iterates the entries: `ParseRDSK`/`ParsePART`
      (RDB), `ParseMBR`/`parse_extended` (MBR/EBR chains), `ParseGPT`,
      or the whole-medium CD/superfloppy mount.

5.  **Filesystem Loading & Mounting**:
    * **RDB path**: `ParsePART` reads the partition's `DosEnvec`, then
      `ParseFSHD` finds or loads the filesystem — `FileSystem.resource` is
      searched and, if absent or older, the filesystem is loaded from the
      disk's FSHD blocks and relocated (`fsrelocate`).
    * **Legacy/CD paths**: `mount_recipe()` builds the `DeviceNode` from the
      recipe — a registered dostype resolves via `FileSystem.resource`,
      otherwise the recipe's handler file is attached (`dn_Handler`,
      `dn_GlobalVec = -1`) for DOS to load on first access. With
      `MOUNTFS_FORCELOAD` the handler file is tried first and the resource
      entry becomes the fallback. Partitions whose recipe resolves to neither
      are skipped before any node is created.
    * The `DeviceNode` is created with `MakeDosNode()` and added via
      `AddBootNode()` (`AddMountNode`), with a `ConfigDev` only when the mount
      is bootable and pre-DOS — the same rule on every path; `MSF_NO_BOOT`
      forces non-bootable.

6.  **Cleanup & Result**:
    * All allocated resources (I/O requests, ports, library bases) are freed.
    * Return value: total partitions mounted (> 0), 0 if media was recognized
      but nothing mounted, -1 if nothing was recognized on any unit.

---

## 4. Core Functionality Details

### 4.1. Filesystem Relocation (`fsrelocate`)

A critical function is `fsrelocate`, which loads Amiga HUNK-formatted
filesystem binaries from disk into memory.
1.  It starts by reading a `HUNK_HEADER`.
2.  It pre-allocates memory for all hunks (`HUNK_CODE`, `HUNK_DATA`,
    `HUNK_BSS`) defined in the header.
3.  It reads each hunk's data from the LSEG (Load Segment) blocks on disk.
4.  It processes relocation hunks (`HUNK_RELOC32`) to resolve memory address
    pointers within the loaded code and data, making the filesystem executable
    from its new location in RAM.
5.  Finally, it performs a `CacheClearU()` to ensure instruction caches are
    flushed before the OS attempts to execute the newly loaded code.

### 4.2. Filesystem Resource Management (`FSHDProcess`, `FSHDAdd`)

The mounter interacts carefully with the central `FileSystem.resource`.
* `FSHDProcess` is responsible for checking if a required filesystem
  (identified by `DosType`) is already present in the resource list.
* If a filesystem is not present, or if the version on disk is newer than the
  one in memory, it allocates a new `FileSysEntry`.
* After a filesystem is successfully loaded and relocated by `fsrelocate`,
  `FSHDAdd` adds the new `FileSysEntry` to `FileSystem.resource`,
  making it available for all subsequent mounting operations
  system-wide. If loading failed, the entry is freed and the mounter falls
  back to any already-registered filesystem for that dostype.
* `FileSystem.resource` is a Kickstart resident from V40 on, so it is taken as
  given: if it is somehow absent, nothing is registered and nothing is looked
  up. The mounter deliberately does not fabricate one — a second
  `FileSysResource` on `SysBase->ResourceList` would shadow the real one for
  everything that mounts later.

### 4.3. Autoboot Process

Every node goes in through `AddBootNode()`, which covers both worlds: pre-V36's
`AddDosNode()` is defined as `AddBootNode()` with a NULL `ConfigDev`, and a NULL
`ConfigDev` is exactly what a non-bootable node wants.

A `ConfigDev` is passed only when the mount happens before DOS exists and the
partition is bootable — that is what makes the node `NT_BOOTNODE` and so visible
to `strap`. If the caller has no autoconfig board behind the drive, the mounter
creates a "fake" `ConfigDev`, which is what lets a USB or NVMe drive be booted
from at all.

### 4.4. Untrusted-Media Hardening

Everything read from the medium is treated as untrusted (USB sticks arrive
with corrupt or hostile metadata):
* The RDB environment vector's `de_TableSize` and the partition name's length
  byte are clamped before use.
* The PART and FSHD block chains are cycle-capped, so a corrupt `pb_Next` /
  `fhb_Next` loop cannot hang the caller.
* The hunk relocator bounds hunk counts, per-hunk sizes and relocation offsets
  against 32-bit overflow.
* Block checksums bound their summed-longs count to the sector size; MBR/GPT
  tables must pass sanity/CRC checks before being believed.

---

## 5. Dependencies & Diagnostics

The Mounter relies on the following standard AmigaOS libraries:

* `exec.library` (v40+): For memory management, tasking, ports, and core system
  functions.
* `expansion.library` (v40+): For creating `DeviceNode` structures and adding
  boot nodes.
* `dos.library` (v40+), **optional**: only for the device/volume/assign name
  collision check and the handler-file existence check. Opening it is allowed to
  fail — that is precisely how the mounter detects that it is running before DOS
  exists, which puts it on the boot path.

Diagnostics: build with `-DMOUNTER_LOG` and provide
`void mounter_log(const char *fmt, ...)` to receive all mounter output. Format
strings use `%l`-sized conversions only, so a RawDoFmt-based sink formats them
correctly. Without `MOUNTER_LOG`, all output compiles away.

Add `-DMOUNTER_TRACE=1` on top of `-DMOUNTER_LOG` for verbose per-step tracing
(RDB/partition/hunk-loader progress), layered on the same `mounter_log` sink.
Off by default; still compiles away entirely if `MOUNTER_LOG` is undefined.

---

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
