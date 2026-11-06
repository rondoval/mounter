# Changelog — neptune-fixes fork

This fork began as the a4091-software mounter (`main`). Its branch was called
`poseidon-fixes` until 2026-10-19, when the USB stack it serves was renamed
from "Poseidon for AmigaOS" to Neptune. It requires AmigaOS 3.1 (V40) and up.

**It no longer tracks upstream.** It stayed mergeable for a while, but the
2026-08-21 restructure took it past the point where upstream changes apply;
anything worth sending back has to be ported by hand.

Releases are newest first, each in the same groups: Added, Changed, Fixed,
Removed, Internal (code structure, no change in behaviour).

## v1.1


### Added

- **Virtual drives.** An MBR partition of type `0x76`, primary or logical, or a
  GPT partition of Emu68's type `3F82EEBC-87C9-4097-8165-89D6540557C0` holds a
  whole RDB disk, as on Emu68's SD card. The mount pass leaves it alone,
  deciding before the boot-sector sniff, so a fresh one with a stale FAT boot
  sector is not mounted on its parent, and an `RDSK` inside one is not taken
  for the parent's.
- **The virtual-drive device** (`vdrive.h`, built from `VDRIVE_SOURCES`). Each
  host has one, under a name of its own (`nvmevd.device`, `usbscsivd.device`),
  created by the first virtual drive found and so never made on a system
  without any; each virtual drive is a unit of it, addressed from the
  partition's first block, so the inner RDB mounts, boots and partitions
  exactly as on Emu68. See README section 5.
  - **Units:** `VDRIVE_UNIT(parent, slot)`, ten slots per parent unit, which
    HDToolBox shows as address and LUN; a unit is at most 2^32 blocks, with
    brcm-emmc's geometry. A virtual drive past the tenth, or larger, gets no
    unit and stays an ordinary partition of the parent. Only media whose block
    size is a power of two from 256 to 4096 bytes hold virtual drives.
  - **Forwarding:** a unit's I/O goes to the parent unit as 64-bit trackdisk
    requests of the device's own, from a pool, so nothing is allocated per
    request. `AbortIO()` is passed on to the parent, which takes a request off
    its queue if it has not started it; both hosts can.
  - **`HD_SCSICMD`** is emulated for HDToolBox and other partitioning tools:
    TEST UNIT READY, REQUEST SENSE, INQUIRY, READ CAPACITY (10) and (16) with
    SBC-2's PMI (the end of the LBA's track, HDToolBox's track size), MODE
    SENSE pages 3 and 4, READ, WRITE, WRITE AND VERIFY, VERIFY and SYNCHRONIZE
    CACHE, with sense data for every refusal.
  - **Write protection on the parent:** the blocks of every virtual drive with
    a unit are read-only to everyone but the virtual device, so no two paths
    write the same blocks. The host asks `VDriveWriteAllowed()` and
    `VDriveScsiAllowed()` on its write paths, with the unit's handle
    (`struct VDriveParent *`, set by `VDriveRefresh()`); a unit that never had
    a virtual drive passes on a NULL test. `VDriveAnyFenced()` covers commands
    that wipe the whole device.
  - **Completion:** the host hands every finished request to
    `VDriveComplete()`, from a task, and replies only on FALSE; the device's
    own requests complete on the spot. Measured on the CM4, a request through
    the virtual device costs 1 µs more than a direct one.
  - **Locking:** everything the device shares is guarded by `Forbid()` and
    touched from tasks only; nothing takes `Disable()`, which on a PiStorm is a
    write across the Amiga bus. A write check costs 0.2 µs.
  - nvme.device and usbscsi.device both host it: `nvme_reply()` and
    `nReplyIO()` on every reply path, the handle in `NVMeUnit.vdParent` and
    `ncm_VDriveParent`.
- **Licensing in GitHub's standard form**: a `LICENSE` file with the full
  BSD-2-Clause text (upstream's headers have the two conditions, but not the
  disclaimer they refer to) and every copyright holder, this fork's included,
  and an `SPDX-License-Identifier: BSD-2-Clause` line in every source file.
  Each header points to `LICENSE`.
- **`mounter.cmake`** lists the sources (`MOUNTER_SOURCES`,
  `MOUNTER_C_SOURCES`, `VDRIVE_SOURCES`); hosts include it rather than naming
  the files.

### Changed

- **Stricter on malformed tables**: a GPT entry must be 128 × 2^n bytes
  (UEFI); an EBR slot or link with a zero start is unused, which also ends a
  chain that links back to its first EBR.
- **CD**: the ISO 9660 descriptor is read at the unit's block size like any
  other block, which also gives it `MSF_SLOW_SPINUP`'s retries; the table of
  contents goes into a scratch sector. The classic CD01/CDVD recipe, like
  FAT's, is filled in once by `MountDrive()`.
- `mounter.h`'s `MOUNTFS_*` enum is named, `enum MountFSKind`. Source
  compatible.
- The README is the guide for drivers that call the mounter; how it works is in
  the new ARCHITECTURE.md.

### Fixed

- **Hunk loader:**
  - The segment list linked each hunk to the one *before* it, so `fse_SegList`
    reached the first hunk only; it runs forward now, as `LoadSeg()` builds it.
  - Each segment's size long is in bytes, as `UnLoadSeg()` expects; it held
    longs.
  - `HUNK_RELOC32SHORT` read its count and hunk number as longs and lost the
    word after each long read; it is words throughout now, and the pad word at
    its end is dropped. `HUNK_DREL32` (1015), what V37 `LoadSeg()` takes for it
    in a load file, is accepted too.
  - Filesystems stored unstripped load: `HUNK_NAME`, `HUNK_SYMBOL` and
    `HUNK_DEBUG` are skipped as `LoadSeg()` does, and an unknown hunk marked
    advisory as V39 does; the whole file was refused, and the partition fell
    back to whatever `FileSystem.resource` had. Memory bits on a hunk's type
    are accepted, and a header whose hunk table does not hold its last hunk is
    refused.
  - An LSEG block carries `lsb_SummedLongs - 5` longs of data, not a fixed 123,
    so 1024- and 2048-byte media with whole-block LSEGs load.
  - A looping LSEG chain ends: a block without data stops the load, and one load
    reads at most 65536 blocks.
  - A loaded filesystem that is not registered is freed, code and all; only its
    entry was.
- **Filesystems:**
  - `MOUNTFS_FORCELOAD` falls back to the resource entry when the handler file
    is missing, as the flag always promised. The handler path was taken without
    looking for the file, so once DOS was up, a machine with the filesystem in
    ROM only got a CD device that could not start.
  - A filesystem loaded off two disks at once is registered once: the check
    against `FileSystem.resource` is repeated under the `Forbid()` of the
    `AddHead()`, and the second copy gives way.
- **RDSK checksum**: the RDB itself is checksummed like its PART, FSHD and LSEG
  blocks, and a summed-longs count below 3 (which checked nothing) fails.
- **Mounting:**
  - A recipe's handler and control BSTRs are allocated after the duplicate
    check and freed if `MakeDosNode()` fails; they leaked on every
    already-mounted pass.
  - `AddBootNode()`'s result is checked: a refused node is a failed mount, not
    a counted one.
  - The fake `ConfigDev` is made on the first bootable volume of a pre-DOS call,
    not up front: a call that mounts nothing bootable allocates none.
  - A volume is named only once its filesystem is resolved, so `renamed` never
    counts a volume that then fails to mount; one that waits for DOS is a
    `MOUNT_DEFERRED` outcome.
  - A name that cannot be bumped any further fails the mount instead of being
    used unchecked; `renamed` counts a renamed volume once, not each bump.
  - The duplicate check compares the whole geometry (block size, surfaces,
    blocks per track) with the cylinders, and believes a startup field only
    when it lies in RAM: SER:, PAR: and PRT: keep an integer there.
  - A BootNode without a DeviceNode no longer crashes the name check.
  - A NULL `creatorName` names the device, as `mounter.h` says; it gave "".
- **CD**: a recognized disc whose filesystem has to wait for DOS, or is
  missing, reports "recognized, nothing mounted" (0), not "unrecognized" (-1).
- **GPT**: the entry array is read at its 64-bit block number; it was cut to 32
  bits.
- **Blocks under 512 bytes** hold no MBR, GPT or VBR; the checks no longer read
  past them.

### Internal

- **Nine units, calls one way.** `mounter.c` keeps the entry point only. The
  block reads and the sector pool are in `mounter_io.c`; the duplicate guard,
  DOS naming and boot node in `mounter_volume.c`; `FileSystem.resource`, recipe
  resolution and the filesystem attach in `mounter_fs.c` (`mnt_resolve_fs()`,
  run by `mnt_mount_volume()` itself); the hunk loader in `mounter_hunk.c`;
  MBR, EBR and GPT parsing in `mounter_table.c`, the only includer of
  `legacy.h`. `mounter_internal.h` has one section per unit.
- **The partition table is read once per unit, before anything mounts**
  (`mnt_read_table()` into `md->table`): the RDB scan's virtual-drive check is
  a lookup in it, with no I/O; the legacy scan is a loop over its data entries,
  a superfloppy being its one entry; `mnt_find_virtual_drives()`, beside
  `MountDrive()` and with the same open, geometry and close as a mount pass,
  lists the virtual drives from it; one dump logs it.
- `mnt_fse_new()` takes no `Forbid()`: the lookup has its own, and the entry
  is private until it is registered. One PatchFlags copy serves both the
  FSHD-to-entry and the entry-to-node direction, with the field layout asserted
  at compile time. The `FileSysEntry` is `AllocVec()`'d. `RDB_END` names the
  end of an RDB chain. Raw and checked RDB block reads are two functions, the
  hunk loader is four, and the DOS name's number is bumped by one of its own
  (`bump_number()`). `MountData` is grouped by lifetime and holds every total.
  Enum values are held in their enum types, and the log macro is `mnt_log()`,
  not a `printf` that only looked like stdio.
- **The virtual-drive device is six units, calls one way:** the host's
  lifecycle calls (`vdrive.c`), the write checks (`vdrive_fence.c`), the exec
  device (`vdrive_device.c`), the requests to the parent and their completion
  (`vdrive_forward.c`), the SCSI target (`vdrive_scsi.c`), and the parent and
  unit records (`vdrive_unit.c`). `vdrive_internal.h` maps them and states the
  locking and the naming once: the host's names start `VDrive`, what the units
  share `vd_`, `VD` or `VD_`, a unit's own carry no prefix, and every function
  names its object first, in full words.
- **The SCSI target does no I/O.** A command that needs the parent comes back
  as a wrapped request (`struct VDWrappedIO`) drafted on the caller's stack,
  sent like a trackdisk one by the one function that also fails the client's
  request when the pool is empty. The trackdisk block commands are cases of
  BeginIO's switch, each naming what it becomes on the parent.
- **The SCSI wire formats are structures** (`vdrive_scsi.h`): every CDB the
  target or the fence reads, UNMAP's parameter list, and every response,
  packed and in the 68k's own byte order, so a field reads and writes as it is.
  Opcodes, status, sense keys and sense codes have their standard names, the
  CDB decoder is one switch of opcodes, and the sizes the standards give are
  checked at compile time. No number stands for a meaning: the `ETD_` commands
  are listed by name, and brcm-emmc's geometry has names.
- Every function carries a kernel-doc comment: what it does, each parameter,
  the return value.

## v1.0 (2026-11-03, 20e5b26)

The fork's first release: what it changed against upstream (A4091/mounter
`main`, with `MountDrive(struct MountStruct *)`), for nvme.device and Neptune's
usbscsi.device.

### Added

- **Filesystem recipes** (`struct MountFS`; `fs[MOUNTFS_FAT]`,
  `fs[MOUNTFS_NTFS]`, `fs[MOUNTFS_EXFAT]` and `fs[MOUNTFS_CD]` in
  `MountStruct`): per filesystem family, the dostype, an optional handler file
  (e.g. `L:fat95`), the preferred DOS device name, `de_Control`, buffers,
  MaxTransfer and stack size.
  - A dostype `FileSystem.resource` does not have mounts through the handler
    file (`dn_Handler`), which DOS loads on first access; a recipe that
    resolves to neither is skipped, and so is one whose handler file cannot be
    found once DOS is up.
  - `fsFlags`: `MOUNTFS_FORCELOAD` tries the handler file before a registered
    dostype, as `ForceLoad = 1` does in a mountlist; `MOUNTFS_CD_AUDIO` mounts
    audio-only discs; `MOUNTFS_CD_ANYFMT` leaves identifying the disc's format
    to the handler (High Sierra, UDF, HFS besides ISO 9660).
  - Without recipes the classic behaviour holds: FAT through
    `FileSystem.resource` only, no NTFS, CD01 or CDVD for CDs.
- **NTFS, exFAT and superfloppies**: a partition's boot sector is sniffed and
  FAT, NTFS or exFAT mounts through its recipe; exFAT is recognized by its
  signature and the fields the spec pins down. Unknown content is skipped,
  where upstream mounted everything as FAT. A filesystem at block 0 mounts the
  whole medium.
- **CDs after boot**: data discs mount through the CD recipe, Amiga-bootable
  ones ("AMIGA BOOT", "CDTV") with boot priority, RDB-formatted ones as before;
  the table of contents is believed only on a clean status.
- **`flags`**: `MSF_NO_RDB`, `MSF_NO_LEGACY` and `MSF_NO_CD` skip a scan;
  `MSF_LEGACY_FIRST_ONLY` mounts only the first MBR/GPT/superfloppy filesystem
  of a unit; `MSF_NO_BOOT` makes no boot nodes; `MSF_SLOW_SPINUP` and
  `MSF_IGNORE_LAST`.
- **Results** (`struct MountResult` and `unitResults`): the volumes mounted,
  deferred, already mounted and renamed, whether the medium was recognized, and
  one result per unit. `MountDrive()` returns the volumes mounted across all
  units, 0 for recognized with nothing mounted, -1 for nothing recognized;
  upstream counted no MBR, GPT or CD mount and kept only the last unit's
  status.
- **Unique DOS names**: a trailing digit is ensured and bumped past
  collisions, checked against the boot-time mount list and the live DOS lists,
  so a second stick never takes the first one's name; RDB partition names too.
- **Pre-DOS nodes boot**: the fake `ConfigDev` carries `ERTF_DIAGVALID` and a
  `DiagArea` whose boot point (`bootpoint.S`) is what the RKRM prescribes,
  `FindResident("dos.library")` and a jump to its `rt_Init`. A boot node alone
  put a volume in the boot menu but never booted it.
- **`dmaAlign`**: the device's preferred DMA alignment; nonzero puts the
  recipe-mounted filesystems' buffers in Fast RAM.
- **Logging**: `MOUNTER_LOG` sends the mounter's output to a `mounter_log()`
  the host provides, its format strings safe for RawDoFmt sinks;
  `MOUNTER_TRACE` adds per-step tracing.

### Changed

- **One entry point, `MountDrive(const struct MountStruct *ms, struct
  MountResult *res)`**, taking an explicit list of units (`units`,
  `unitCount`); `MountStruct` is pure input. Breaking: upstream's `unitNum` (a
  pointer, or a unit number below 0x100), `slowSpinup`, `ignoreLast`, `cdBoot`,
  `luns` and `hostId` are gone.
- **Exact partition extents**: MBR, GPT and superfloppy volumes map block for
  block, one block a "cylinder". Upstream's CHS fitting put an unaligned
  partition (a classic LBA-63 MBR) one block off, which corrupted data on
  write.
- **MBR and GPT**: extended partitions `0x0F` and `0x85` are accepted and EBR
  chains walked with their container-relative links; an implausible MBR is
  refused; a GPT is believed only behind a protective MBR entry and with a
  valid header (position, size, CRC32), honours `size_of_entry`, takes at most
  128 entries and skips a partition past 2^32 blocks instead of truncating it.
- **Large media**: reads past 4 GB use `TD_READ64`; sector sizes from 256 to
  4096 bytes are accepted, and any other is refused per unit instead of
  overflowing a buffer.
- **Hardened against corrupt media**: the RDB environment vector, the drive
  name's length, hunk counts and sizes, and relocation offsets are clamped;
  PART and FSHD chains are cycle-capped; a failed filesystem load leaves no
  `FileSystem.resource` entry behind.
- **One boot-node rule**: a `ConfigDev` goes with a node only before DOS, and
  only for a bootable partition, on every path.
- **Filesystem buffers**: `de_Mask` stays word-aligned whatever `dmaAlign` says,
  since no filesystem honours more and the device bounces misaligned buffers;
  `de_BufMemType` is `MEMF_FAST | MEMF_PUBLIC`, without `MEMF_CLEAR`, an
  allocation flag fat95's buffer test never passed with.

### Fixed

- **The duplicate-extent guard matches**: `MakeDosNode()` counts the BSTR's
  terminating NUL, which the comparison did not allow, so a replugged stick
  whose node had stayed came back as `UMSD1:` beside `UMSD0:`.
- **`renamed` is not inflated** by the very case it exists to detect: the
  extent is checked before the name is fixed.
- One unit's mount count no longer leaks into the next; MBR, GPT and CD nodes
  get `de_TableSize` 16, or 19 with `de_Control`, not 80; the motor is switched
  off with the right request after a failed geometry; a `FileSysEntry` is freed
  with its own size when a load fails.

### Removed

- **Pre-3.1 Kickstart support**: the `W_*` port and request shims (exec's own
  V36 calls instead), the version gates, the Kickstart 1.3 boot-node path,
  making a `FileSystem.resource` of its own, `copymem()`. 68000 support stays.
- **The blind SCSI scan** (`ScanAllUnits()`, `luns`, `hostId`), which neither
  consumer could use; `cdBoot`, replaced by `MSF_NO_CD`; the `DISKLABELS`
  compile gate, replaced by `MSF_NO_LEGACY` at run time; `ndkcompat.h`; the
  MS0-MS9/CD0-CD9 name probing; dead logging knobs.

### Internal

- **Four sources** (`mounter.c`, `mounter_rdb.c`, `mounter_legacy.c`,
  `mounter_cd.c`) and `mounter_internal.h`, with one path that creates a
  DeviceNode (`mnt_mount_volume()`), one sector pool and one result
  convention. `PascalCase` is the API, `mnt_` what crosses a file, plain
  `snake_case` what stays in one.
- **Warning-clean** under `-Wall -Wextra -Wconversion -Wsign-conversion -Wshadow
  -Wmissing-prototypes -Wstrict-prototypes`, which both consumers hold it to.
