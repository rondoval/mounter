# Mounter: AmigaOS Generic Mounter

Generic autoboot / automount partition parser and mounter: RDB, MBR, GPT,
superfloppy and data CDs.

Originally the mounter code from `a4091.device`; this is the `poseidon-fixes`
fork, which stopped tracking upstream and has since been restructured. See
CHANGELOG.md for its history.

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

It is built from four sources: `mounter.c` (entry, per-unit dispatch, and the
single path that creates a `DeviceNode`), `mounter_rdb.c` (RDB, FSHD, hunk
loader), `mounter_legacy.c` (MBR, EBR chains, GPT, superfloppy) and
`mounter_cd.c` (CD/DVD/BD). `mounter_internal.h` carries what they share.

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
      that identifies formats itself (`MOUNTFS_CD_ANYFMT`) is handed any data
      disc, and one that understands audio tracks (`MOUNTFS_CD_AUDIO`) is handed
      audio-only discs too. Both are properties of the handler, so they are set
      in the recipe's `fsFlags`.
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
* **Explicit unit mounting**: the caller passes a `units` array and a
  `unitCount`, and optionally a `unitResults` array to receive each unit's
  outcome. The input is never written to. (This fork dropped upstream's blind
  SCSI target/LUN scan; its consumers are not SCSI hosts and always know their
  units.)
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
    * `units`, `unitCount`: the unit numbers to probe. Both required.
    * `unitResults`: OPTIONAL, `unitCount` entries, filled in with each unit's
      result (-1 nothing recognized, 0 recognized but nothing mounted, >0 the
      count, -2 skipped after an earlier `RDBFF_LAST`). NULL if not wanted.
    * `creatorName`: A string to identify the creator of the filesystem entries.
    * `configDev`: A pointer to the `ConfigDev` structure for an autoconfig
      board, essential for autobooting.
    * `flags`: `MSF_*` flags gating the RDB / MBR-GPT-superfloppy / CD scans,
      boot-node creation, spin-up patience (`MSF_SLOW_SPINUP`) and whether
      `RDBFF_LAST` ends the scan (`MSF_IGNORE_LAST`).
    * `fs[]`: filesystem recipes indexed by `MOUNTFS_FAT` / `MOUNTFS_NTFS` /
      `MOUNTFS_EXFAT` / `MOUNTFS_CD` (see below); a NULL entry means that
      family is not mounted, except FAT, which falls back to the classic
      dostype from `FileSystem.resource`.
    * `SysBase`: A pointer to the Exec library base.

  The struct is pure input — the mounter never writes to it.

* **`struct MountResult` (`mounter.h`)**: OPTIONAL second argument to
  `MountDrive()`. Reports what the call did: `mounted`, `deferred` (volumes
  whose handler needs a DOS that did not exist yet), `alreadyMounted` (extents
  left alone because they were already mounted), `renamed` (DOS names bumped
  past a collision) and `recognized`. Pass NULL if none of it is wanted.

* **`struct MountFS` (`mounter.h`)**: A filesystem recipe for non-RDB media:
  dostype, optional handler file, preferred DOS device name, `de_Control`
  string, buffers, MaxTransfer, handler stack size and `MOUNTFS_*` flags.
  Those flags describe what the *handler* can do — `MOUNTFS_FORCELOAD`, and for
  CD handlers `MOUNTFS_CD_AUDIO` / `MOUNTFS_CD_ANYFMT` — so they travel with
  the recipe rather than with the session. All strings are owned by the caller
  for the duration of `MountDrive()`.

* **`struct Volume` (`mounter_internal.h`)**: One mountable volume as a scanner
  found it — the `DosEnvec`, a name hint, a boot priority, and either a `MountFS`
  recipe (CD and MBR/GPT paths) or a pre-resolved `FileSysEntry` (RDB path).
  This is what every scanner hands to `mnt_mount_volume()`.

* **`struct MountData` (`mounter_internal.h`)**: An internal state-management structure
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
    * Probe exactly the given unit(s) (`scan_units`), reporting each unit's
      result into `unitResults` if one was supplied and honoring `RDBFF_LAST`
      unless `MSF_IGNORE_LAST` is set.
    * Each unit is probed by `probe_unit()`: `OpenDevice()`, geometry, scan,
      motor off, `CloseDevice()`. Each scanner answers only "did you recognize
      this medium?"; `probe_unit()` turns that plus the count into the
      -1/0/count the caller sees.

3.  **Partition Scheme Identification** (per unit):
    * Get the geometry via `TD_GETGEOMETRY`; sector sizes 256–4096 are
      accepted.
    * **Direct-access devices**: try RDB first — scan the first
      `RDB_LOCATION_LIMIT` blocks (`mnt_scan_rdb`, unless `MSF_NO_RDB`). If no RDB,
      classify block 0 (`mnt_scan_legacy`, unless `MSF_NO_LEGACY`): a GPT (gated by
      its protective MBR entry and a validated header at block 1), else a
      filesystem VBR at block 0 (superfloppy), else a sane MBR.
    * **CD/WORM/optical devices** (unless `MSF_NO_CD`): check unit ready, then
      classify the disc from its TOC (`classify_cd`: data track 1, audio track 1,
      or unreadable). For a data disc, read the ISO PVD (`read_pvd`) — used for
      "AMIGA BOOT"/"CDTV" boot priority only. No PVD → RDB-CD fallback, then
      mount anyway if the recipe declares `MOUNTFS_CD_ANYFMT` (an unreadable
      sector 16 is still refused). An unreadable TOC is likewise treated as a
      data disc under `MOUNTFS_CD_ANYFMT`, since some enclosures answer READ TOC
      poorly for DVD/BD media.

4.  **Partition Processing**:
    * The appropriate parser iterates the entries: `parse_rdsk`/`parse_part`
      (RDB), `parse_mbr`/`parse_ebr` (MBR/EBR chains), `parse_gpt`,
      or the whole-medium CD/superfloppy mount. Each of them ends by filling a
      `struct Volume` and calling `mnt_mount_volume()`.
    * Scratch sectors come from one four-slot LIFO pool
      (`mnt_sector_take`/`mnt_sector_drop`) allocated on first use and released
      when `MountDrive()` returns.

5.  **Filesystem Loading & Mounting**:
    * **RDB path**: `parse_part` reads the partition's `DosEnvec`, then
      `parse_fshd` finds or loads the filesystem — `FileSystem.resource` is
      searched and, if absent or older, the filesystem is loaded from the
      disk's FSHD blocks and relocated (`fs_relocate`).
    * **Legacy/CD paths**: the scanner resolves a recipe (`mnt_resolve_fs`) and
      synthesises the `DosEnvec` from the block extent
      (`mnt_envec_from_recipe`) — a registered dostype resolves via
      `FileSystem.resource`,
      otherwise the recipe's handler file is attached (`dn_Handler`,
      `dn_GlobalVec = -1`) for DOS to load on first access. With
      `MOUNTFS_FORCELOAD` the handler file is tried first and the resource
      entry becomes the fallback. Partitions whose recipe resolves to neither
      are skipped before any node is created.
    * **Both paths then converge on `mnt_mount_volume()`**, the only function
      that creates a node. In order: the duplicate-extent guard, the DOS name
      (seeded and bumped past collisions), `MakeDosNode()`, the filesystem
      attach, and `AddBootNode()` (`add_mount_node`) with a `ConfigDev` only
      when the mount is bootable and pre-DOS. `MSF_NO_BOOT` forces
      non-bootable. The extent guard runs first, so a volume already mounted on
      an earlier pass never reaches the naming step.

6.  **Cleanup & Result**:
    * All allocated resources (I/O requests, ports, library bases) are freed.
    * Return value: total partitions mounted (> 0), 0 if media was recognized
      but nothing mounted, -1 if nothing was recognized on any unit.

---

## 4. Core Functionality Details

### 4.1. Filesystem Relocation (`fs_relocate`)

A critical function is `fs_relocate` (`mounter_rdb.c`), which loads Amiga HUNK-formatted
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

### 4.2. Filesystem Resource Management (`fse_from_fshb`, `fse_register`)

The mounter interacts carefully with the central `FileSystem.resource`.
* `mnt_find_filesystem` is the single lookup: given a dostype, it returns the
  registered `FileSysEntry` or NULL.
* `fse_from_fshb` builds a new `FileSysEntry` from an RDB `FileSysHeaderBlock`,
  unless the resource already carries an entry for that dostype at the same
  version or newer. Both the dostype and the version come out of the header
  block, so there is nothing for the caller to pass alongside it.
* After a filesystem is successfully loaded and relocated by `fs_relocate`,
  `fse_register` adds the new `FileSysEntry` to `FileSystem.resource`,
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
