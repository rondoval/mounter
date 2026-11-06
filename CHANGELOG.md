# Changelog — neptune-fixes fork

This fork began as the a4091-software mounter (`main`). Its branch was called
`poseidon-fixes` until 2026-10-19, when the USB stack it serves was renamed
from "Poseidon for AmigaOS" to Neptune. It requires AmigaOS 3.1 (V40) and up.

**It no longer tracks upstream.** It stayed mergeable for a while, but the
2026-08-21 restructure took it past the point where upstream changes apply;
anything worth sending back has to be ported by hand.

Entries are newest first, dated by commit, each in the same groups: Added,
Changed, Fixed, Removed, Internal (code structure, no change in behaviour).
Older entries use the names of their time; the key below maps the ones that
changed.

| In older entries | Now |
|---|---|
| `fatFS`, `ntfsFS`, `exfatFS`, `cdFS` | `fs[MOUNTFS_FAT]`, `fs[MOUNTFS_NTFS]`, `fs[MOUNTFS_EXFAT]`, `fs[MOUNTFS_CD]` |
| `unitNum` | `units`, `unitCount`, `unitResults` |
| `MSF_CD_AUDIO`, `MSF_CD_ANYFMT` | `MOUNTFS_CD_AUDIO`, `MOUNTFS_CD_ANYFMT` |
| `slowSpinup`, `ignoreLast` | `MSF_SLOW_SPINUP`, `MSF_IGNORE_LAST` |
| `DetectVBR` | `mnt_vbr_kind()` |
| `ClassifyCD()`, `CheckPVD()` | `classify_cd()`, `read_pvd()` |
| `SaneMBR` | `is_sane_mbr()` |
| `AddNode()`, `AddLegacyNode()`, `AddMountNode()` | `add_mount_node()` |
| `register_legacy()`, `ScanCDROM` | `mount_partition()`, `mnt_scan_cd()` |
| `parse_mbr()`, `parse_ebr()`, `parse_gpt()`, `mnt_in_container()` | `mounter_table.c`: the partition table, read once per unit |
| `fs_relocate()`, `fse_from_fshb()`, `fse_register()` | `mnt_load_lseg()`, `mnt_fse_new()`, `mnt_fse_register()` |
| `printf()` (the log macro) | `mnt_log()` |
| `mnt_find_containers()`, `struct MountContainer` | `mnt_find_virtual_drives()`, the table's `struct Partition` |
| `VDriveAnyContainers()` | `VDriveAnyFenced()` |

## 2026-11-04 — the virtual-drive device in six units

### Added

- **`VDriveComplete(vd, io)`**: a host hands every finished request to it, from
  a task, before replying, and replies only on FALSE; the virtual device's own
  requests complete on the spot, and come back no other way. It replaces the
  software interrupt that completed them, and the `Disable()` that interrupt
  needed: measured on the CM4, a request through the virtual device costs
  1 µs more than a direct one instead of 57 µs. nvme.device (`nvme_reply()`)
  and usbscsi.device (`nReplyIO()`) call it on every reply path.

### Changed

- **A host names a parent unit by a handle** (`struct VDriveParent *`), kept
  with its own unit: `VDriveRefresh()` takes a fourth argument and sets it
  once the unit has had a virtual drive, and `VDriveParentGone()`,
  `VDriveWriteAllowed()` and `VDriveScsiAllowed()` take it in place of the
  device and the unit number. The write checks no longer search the device's
  list of parents, and a unit that never had a virtual drive passes on a NULL
  test even while another unit has some. nvme.device keeps it in
  `NVMeUnit.vdParent`, usbscsi.device in `ncm_VDriveParent`.
- **`AbortIO()` on a virtual unit reaches the parent**: a read or write the
  client aborts is aborted on the parent unit too, which takes it off its
  queue if it has not started it (nvme.device and usbscsi.device both can), and
  the request comes back with `IOERR_ABORTED`; an emulated `HD_SCSICMD` does
  too, without sense data. Only change interrupts could be aborted before.
- **READ CAPACITY (10) and (16) on a virtual unit follow SBC-2's PMI**: without
  PMI the LBA must be 0 (else invalid field in CDB) and the answer is the last
  block, as before; with PMI the answer is the last block of the track holding
  the LBA, in the unit's own geometry. They answered with the drive's last block whatever the CDB said.
- **Only the virtual drives that have a unit are write-protected on the
  parent**: the protection keeps two paths from writing the same blocks, and a
  virtual drive with no unit (past the tenth, or of 2^32 blocks or more) has
  one path only, so it stays an ordinary, writable partition of the parent. Up
  to 16 were protected before, and a parent with more refused every write.
- **`VDriveAnyContainers()` is `VDriveAnyFenced()`**, the last "container" in
  the API; hosts rename the call.
- **`mounter.cmake` lists the virtual-drive device too** (`VDRIVE_SOURCES`);
  a host adds that list instead of naming `vdrive.c`.
- **A medium with blocks over 4096 bytes has no virtual drives**: a rescan
  lifts its fence, as for any medium that cannot hold one. It counted as
  unreadable, which kept the previous medium's fence.
- `vdrive.h` says what both hosts already do: `VDriveRefresh()` and
  `VDriveDestroy()` are called from one task at a time.
- **Faster on a PiStorm, where every `Disable()` writes across the Amiga bus
  (5.6 µs a pair, measured on the CM4).** Everything the device shares is
  guarded by `Forbid()` instead, since `VDriveComplete()` left nothing at
  interrupt level: a unit's state and its change interrupts, the rescans, the
  write checks, and the pool of requests to the parent. A write check on the
  parent costs 0.2 µs, down from 5.9 µs. `AbortIO()` on a virtual unit is
  called from a task, as on both hosts.

### Internal

- **Six units, calls one way**, in place of one file of 1600 lines: the host's
  lifecycle calls (`vdrive.c`), the write checks (`vdrive_fence.c`), the exec
  device (`vdrive_device.c`), the requests to the parent and their completion
  (`vdrive_forward.c`), the SCSI target (`vdrive_scsi.c`), and the parent and
  unit records (`vdrive_unit.c`). `vdrive_internal.h` maps them and states the
  locking and the naming once.
- **The SCSI target does no I/O.** A command that needs the parent comes back
  as a wrapped request drafted on the caller's stack (the forwarded request's
  command, buffer, length and offset, the client's request and its unit), sent
  like a trackdisk one. Sending a copy of the draft, and failing the request
  when no wrapped request can be had, is one function of two arguments; five
  call sites did it each their own way. The trackdisk block commands are cases
  of BeginIO's switch, each naming what it becomes on the parent.
- **The mounter lists a unit's virtual drives itself**:
  `mnt_find_virtual_drives()`, in `mounter.c` beside `MountDrive()`, with the
  same open, geometry and close as a mount pass. The virtual device no longer
  opens the unit or reads its geometry itself, and it takes the table's own
  entries; `struct MountContainer` is gone.
- A parent's fence is its fenced units' own block ranges, with no list of its
  own. The change-interrupt search, the device teardown, and the test of
  whether a request is checked at all are each written once.
- **One naming scheme**: the host's names start `VDrive`, what the units share
  `vd_`, `VD` or `VD_`, a unit's own carry no prefix, and every function names
  its object first, in full words (`device`, `unit`, `parent`, not `vd`,
  `vu`, `p`). A request sent to the parent travels in a `struct VDWrappedIO`,
  wrapped with the client's request it completes. It is a plain `IOStdReq`, 8
  bytes smaller than the `IOExtTD` it was: nothing sent is an `ETD_` command,
  so its `iotd_Count` was never read.
- **The SCSI wire formats are structures** (`vdrive_scsi.h`): every CDB the
  target or the fence reads, UNMAP's parameter list, and every response
  (sense, INQUIRY, READ CAPACITY, the mode pages), packed and in the 68k's own
  byte order, so a field reads and writes as it is and the byte-by-byte
  helpers are gone. Opcodes, status, sense keys and sense codes have their
  standard names, the CDB decoder has one switch of opcodes, and the sizes
  the standards give are checked at compile time.
- No number stands for a meaning any more: the `ETD_` commands are listed by
  name (a bit test also caught the 64-bit NSD commands' bit 15), the slot of a
  unit number is `VDRIVE_SLOT()`, and brcm-emmc's geometry has names. The set-up
  of a port that signals nobody is written once.

## 2026-11-04 — nine units, the partition table read first

### Fixed

- **`MOUNTFS_FORCELOAD` falls back to the resource entry when the handler file
  is missing**, as the flag always promised. The handler path was taken without
  looking for the file, so once DOS was up, a machine with the filesystem in
  ROM only got a CD device that could not start. The three functions that each
  decided part of this are one, `mnt_resolve_fs()`, run by
  `mnt_mount_volume()` itself.
- **A volume is named only once its filesystem is resolved**, so `renamed`
  never counts a volume that then fails to mount. One that waits for DOS is a
  `MOUNT_DEFERRED` outcome.
- **A NULL `creatorName` names the device**, as `mounter.h` says; it gave "".
- **A filesystem loaded off two disks at once is registered once.** The check
  against `FileSystem.resource` ran under a `Forbid()` that ended before the
  load, so two callers could both register it; the check is now repeated under
  the `Forbid()` of the `AddHead()`, and the second copy gives way.
- **A loaded filesystem that is not registered is freed** (`mnt_unload_lseg()`),
  code and all; only its entry was.
- **Filesystems stored unstripped load.** The hunk loader skips `HUNK_NAME`,
  `HUNK_SYMBOL` and `HUNK_DEBUG`, as `LoadSeg()` does, and an unknown hunk
  marked advisory, as V39 does; it refused the whole file, and the partition
  fell back to whatever `FileSystem.resource` had. Memory bits on a hunk's type
  are accepted, and a header whose hunk table does not hold its last hunk is
  refused.
- **A looping LSEG chain ends**: one load reads at most 65536 blocks. Only a
  block without data stopped one before.

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
- **`mounter.cmake`** lists the sources (`MOUNTER_SOURCES`,
  `MOUNTER_C_SOURCES`); hosts include it rather than naming the files.
- The README is the guide for drivers that call the mounter; how it works moved
  to the new ARCHITECTURE.md.

### Internal

- **Nine units, calls one way.** `mounter.c` keeps the entry point only. The
  block reads and the sector pool moved to `mounter_io.c`; the duplicate guard,
  DOS naming and boot node to `mounter_volume.c`; `FileSystem.resource`, recipe
  resolution and the filesystem attach to `mounter_fs.c`; the hunk loader to
  `mounter_hunk.c`; MBR, EBR and GPT parsing to `mounter_table.c`, the only
  includer of `legacy.h`. `mounter_internal.h` has one section per unit.
- **The partition table is read once per unit, before anything mounts**
  (`mnt_read_table()` into `md->table`), instead of walked with a callback by
  each of its three users:
  - the RDB scan's virtual-drive check is a lookup in it, with no I/O; it used
    to read the whole table again for every RDSK found past block 0;
  - the legacy scan is a loop over its data entries; a superfloppy is the
    table's one entry, and `MSF_LEGACY_FIRST_ONLY` ends the loop;
  - `mnt_find_containers()` copies its virtual drives out of it;
  - one dump logs it; the scratch-sector pool needs three slots, not four.
- `mnt_fse_new()` takes no `Forbid()`: the lookup has its own, and the entry
  is private until it is registered. The hunk loader frees a failed load with
  the same `mnt_unload_lseg()`.
- One PatchFlags copy serves both the FSHD-to-entry and the entry-to-node
  direction, with the field layout asserted at compile time. The
  `FileSysEntry` is `AllocVec()`'d. `RDB_END` names the end of an RDB chain.
  Raw and checked RDB block reads are two functions, the hunk loader is four,
  and the DOS name's number is bumped by one of its own (`bump_number()`).
  `MountData` is grouped by lifetime and holds every total. Enum values are
  held in their enum types (partition kind and scheme, filesystem family, CD
  disc class, ISO 9660 result), and the log macro is `mnt_log()`, not a
  `printf` that only looked like stdio.

## 2026-11-04 — virtual drives, and fixes from a review of the whole mounter (97fb278)

### Added

- **MBR `0x76` and Emu68's GPT type are left to a virtual device.** Such a
  partition holds a whole RDB disk, as on Emu68's SD card. The mount pass skips
  it before the boot-sector sniff, so a fresh container with a stale FAT boot
  sector is not mounted on its parent, and `mnt_scan_rdb()` ignores an `RDSK`
  inside one.
- **`vdrive.c` / `vdrive.h`**: the exec device a host creates for them, one
  unit per virtual drive, forwarding to the parent unit, with the write
  protection the host applies on the parent. See README section 5.

### Fixed

- **Hunk loader** (`fs_relocate()`):
  - The segment list linked each hunk to the one *before* it, so `fse_SegList`
    reached the first hunk only; it now runs forward, as `LoadSeg()` builds it.
  - Each segment's size long is in bytes, as `UnLoadSeg()` expects; it held
    longs.
  - `HUNK_RELOC32SHORT` read its count and hunk number as longs and lost the
    word after each long read; it is words throughout now, and the pad word at
    its end is dropped. `HUNK_DREL32` (1015), what V37 `LoadSeg()` takes for it
    in a load file, is accepted too.
  - An LSEG block carries `lsb_SummedLongs - 5` longs of data, not a fixed 123,
    so 1024- and 2048-byte media with whole-block LSEGs load. A block with no
    data ends the load: a cyclic `lsb_Next` could otherwise spin forever.
- **RDSK checksum**: the RDB itself is now checksummed like its PART, FSHD and
  LSEG blocks, and a summed-longs count below 3 (which checked nothing) fails.
- **Mounting** (`mnt_mount_volume()`):
  - A recipe's handler and control BSTRs are allocated after the duplicate
    check and freed if `MakeDosNode()` fails; they leaked on every
    already-mounted pass.
  - `AddBootNode()`'s result is checked: a refused node is a failed mount, not
    a counted one.
  - The fake `ConfigDev` is made on the first bootable volume of a pre-DOS call,
    not up front: a call that mounts nothing bootable allocates none.
  - A name that cannot be bumped any further fails the mount instead of being
    used unchecked; `renamed` counts a renamed volume once, not each bump.
  - The duplicate check compares the whole geometry (block size, surfaces,
    blocks per track) with the cylinders, and believes a startup field only when
    it lies in RAM: SER:, PAR: and PRT: keep an integer there.
  - A BootNode without a DeviceNode no longer crashes the name check.
- **CD**: a recognized disc whose filesystem has to wait for DOS, or is
  missing, reports "recognized, nothing mounted" (0), not "unrecognized" (-1).
- **GPT**: the entry array is read at its 64-bit block number; it was cut to 32
  bits. `mnt_read_block()` takes a 64-bit block.
- **Blocks under 512 bytes** hold no MBR, GPT or VBR; the checks no longer read
  past them.

### Internal

- One table walker: `parse_mbr()`, `parse_ebr()` and `parse_gpt()` handed each
  entry to a callback, the mount pass one, `mnt_find_containers()` another. A
  GPT entry with `last < first` is skipped.
- `sector_pool_free()` is exported as `mnt_sector_pool_free()`.
- Every function carries a kernel-doc comment: what it does, each parameter,
  the return value.

## 2026-11-03 — the duplicate-extent guard matches again (20e5b26)

### Fixed

- **`is_same_extent()` never matched a node of ours.** `MakeDosNode()` counts the
  terminating NUL in the startup packet's device BSTR (`usbscsi.device` has length
  15 there), and the comparison demanded `strlen()`. So a remount of a volume that
  was already mounted always fell through to the naming step: a replugged stick
  whose node had stayed (auto-unmount off) came back as `UMSD1:` beside `UMSD0:`,
  a second handler on the same partition. Both lengths are accepted now: with the
  NUL counted it is compared against the name's own terminator; a BSTR without one
  (Mount, DOSDrivers) compares as before.

## 2026-10-24 — the DMA alignment is no longer imposed on filesystems (c90d44a)

### Changed

- **`de_Mask` stays word-aligned.** With a nonzero `dmaAlign`, recipe-mounted
  filesystems still get their buffers in Fast RAM, but the alignment no longer
  goes into `de_Mask`: filesystems cannot honour it (PFS3 has no aligned
  allocator and raises a requester per buffer, FFS drops to 512-byte reads,
  fat95 to its cluster cache). The device bounces misaligned buffers itself.
- **`de_BufMemType` is `MEMF_FAST | MEMF_PUBLIC`, without `MEMF_CLEAR`**, which
  is an allocation flag, not a memory property: fat95's test that a caller's
  buffer has every `de_BufMemType` bit never passed with it.

## 2026-09-08 — pre-DOS nodes get a real boot point (b4d97e5)

### Fixed

- **Bootable, not merely listed.** `NT_BOOTNODE` got a pre-DOS node into the boot
  menu but never into the boot: `strap` needs a boot block or an autoboot
  `DiagArea`, and a medium with no Amiga boot block has only the latter. The fake
  `ConfigDev` now carries `ERTF_DIAGVALID` and a `DiagArea` pointing at a new
  boot stub, the `FindResident("dos.library")` → `jmp rt_Init` the RKRM
  prescribes. It was `bootpoint.c`, and has been `bootpoint.S` since 2026-09-28
  (acb36c8).

## 2026-08-21 — restructure (f181ffb)

One mount path, four files, one naming convention. Behaviour is unchanged except
where noted; every parse, clamp and retry count is exactly as it was.

### Changed

Breaking; both consumers were updated in the same change.

```c
LONG MountDrive(const struct MountStruct *ms, struct MountResult *res);
```

- `unitNum` (a `ULONG *` that meant a scalar unit when `< 0x100`) is replaced by
  `units` + `unitCount`, with results reported into an optional `unitResults`
  array. `MountStruct` is now pure input — the mounter no longer writes per-unit
  results back over the caller's input.
- The four positional recipe pointers became `fs[]`, indexed by `MOUNTFS_FAT` /
  `MOUNTFS_NTFS` / `MOUNTFS_EXFAT` / `MOUNTFS_CD`.
- `MSF_CD_AUDIO` / `MSF_CD_ANYFMT` became `MOUNTFS_CD_AUDIO` / `MOUNTFS_CD_ANYFMT`
  in `MountFS.fsFlags`: they describe what the *handler* can do, not the session.
- `slowSpinup` / `ignoreLast` became `MSF_SLOW_SPINUP` / `MSF_IGNORE_LAST`.
- The `deferred` / `alreadyMounted` / `renamed` counters left `MountStruct` for
  `struct MountResult`, which also carries `mounted` and `recognized`.

### Fixed

- **`renamed` was inflated by the case it exists to detect.** The CD and
  MBR/GPT paths fixed the DOS name *before* the duplicate-extent check, so a
  second pass over an already-mounted volume bumped the name (and the counter)
  before deciding not to mount anything. The unified path checks the extent first.

### Internal

- **One node-creating site.** Three functions used to build a `DeviceNode`, each
  with its own copy of: duplicate-extent check, DOS naming, `MakeDosNode()`,
  filesystem attach, `AddBootNode()`. The scanners are now pure enumerators —
  each fills a `struct Volume` and calls `mnt_mount_volume()`, which owns that
  whole tail. `mount_recipe()` is gone.
- **One sector pool.** Eight `AllocMem(MAX_BLOCKSIZE)` sites became a four-slot
  LIFO pool (`mnt_sector_take`/`mnt_sector_drop`), allocated on first use and
  freed when `MountDrive()` returns. Eight out-of-memory paths became one, and a
  10-partition RDB now allocates its FSHD sectors once instead of twenty times.
- **One result convention.** `mnt_mount_volume()` returns a `MountOutcome` and
  keeps the counters in step with it; scanners answer only "did you recognize this
  medium?" and leave the count in the context. The `-1`/`0`/count encoding is
  applied once, in `probe_unit()`.
- **`FSHDProcess()` split into `fse_from_fshb()`**, losing an unreachable branch
  and a `newOnly` parameter that served a mode nobody used. Its lookup-only call
  was just `mnt_find_filesystem()` with extra steps.
- **Hunk loader decoupled.** The six `lseg*` fields left `MountData` for a
  `struct LSegStream` owned by `parse_fshd()`.
- **Split into four sources** — `mounter.c`, `mounter_rdb.c`, `mounter_legacy.c`,
  `mounter_cd.c`, plus `mounter_internal.h`. Only ten symbols cross a file
  boundary; they take an `mnt_` prefix, everything else is `static`.
- **One naming convention.** `PascalCase` = public API or an OS call,
  `mnt_snake_case` = crosses a file boundary, `snake_case` = file-local. Linkage
  is now visible at the call site.
- **Warning-clean** under `-Wall -Wextra -Wconversion -Wsign-conversion -Wshadow
  -Wmissing-prototypes -Wstrict-prototypes`. nvme.device's per-file suppression of
  seven warning classes is deleted; both consumers now hold the mounter to that
  set. Five dead `SysBase` declarations and one shadowed variable fell out of it.

## 2026-06-27 to 2026-08-20 — divergence from upstream

What the fork changed against upstream a4091-software before the restructure.

### Added

- **Filesystem recipes** (`struct MountFS`; `fatFS`/`ntfsFS`/`cdFS` in
  `MountStruct`): the caller decides per filesystem family:
  - dostype, optional handler file (e.g. `"L:fat95"`), preferred DOS device name
  - `de_Control`, buffers, MaxTransfer, handler stack size
  - NULL recipes = classic behavior (FAT via FileSystem.resource only; NTFS
    skipped; CD01/CDVD for CDs)
- **Handler loading without FileSystem.resource**: if a recipe's dostype is not
  registered, the DeviceNode gets `dn_Handler` = the recipe's handler file
  (`dn_GlobalVec = -1`), so DOS loads the filesystem on first access.
  Partitions whose recipe resolves to neither are skipped cleanly.
- **`fsFlags` in `MountFS`**: per-recipe behavior
  - `MOUNTFS_FORCELOAD`: try the recipe's handler file *before*
    `FileSystem.resource`, keeping a registered dostype as the fallback. What
    `ForceLoad = 1` does in a mountlist: a controller ROM registering an old
    filesystem under a well-known dostype (`CD01` is the usual one) no longer
    shadows the handler the caller asked for, while a machine carrying that
    handler only in ROM still mounts.
- **`flags` in `MountStruct`**: gate behavior at runtime
  - `MSF_NO_RDB`, `MSF_NO_LEGACY`, `MSF_NO_CD`: skip the three scans
  - `MSF_LEGACY_FIRST_ONLY`: mount only the first MBR/GPT/superfloppy filesystem per unit
  - `MSF_NO_BOOT`: suppress pre-DOS boot nodes everywhere
  - `MSF_CD_AUDIO`: `cdFS` filesystem understands audio-only discs (e.g.
    ODFileSystem presenting tracks as WAV); mount them non-bootable (no PVD
    check) instead of rejecting via data-track TOC gate
  - `MSF_CD_ANYFMT`: `cdFS` filesystem identifies disc formats itself (e.g.
    ODFileSystem: High Sierra, UDF, HFS and HFS+ besides ISO 9660 with Joliet
    and Rock Ridge). A data disc with no ISO 9660 PVD is handed to it instead
    of being rejected, and an unreadable TOC is treated as a data disc.
    Without the flag the classic rule holds: data discs must be ISO 9660,
    which is all a legacy CDFileSystem can read anyway.
- **`dmaAlign` in `MountStruct`** (2026-07-08): the device's preferred DMA
  alignment. Nonzero put recipe-mounted filesystems' buffers in
  `MEMF_FAST | MEMF_PUBLIC` and the alignment into `de_Mask` (relaxed on
  2026-10-24).
- **Explicit unit mounting (hotplug)**: `unitNum` now works as advertised
  - NULL: scan SCSI targets 0–7 (classic behavior)
  - value < 0x100: mount that single unit
  - otherwise: points to `{count, unit...}` array (entries overwritten with per-unit results)
- **NTFS/exFAT support and superfloppy support**: partition boot sectors
  are sniffed (`DetectVBR`)
  - FAT mounts via `fatFS`, NTFS via `ntfsFS`, exFAT via `exfatFS` (unknown
    content is skipped — previously everything mounted as FAT)
  - exFAT is identified by its `"EXFAT   "` signature plus the fields the spec
    pins down (MustBeZero region, BytesPerSectorShift, FAT count), so a stray
    signature cannot claim a partition
  - filesystem at block 0 (superfloppy) mounts as a whole-disk device
- **Handler files are checked before mounting**: a recipe whose dostype is not
  in FileSystem.resource and whose handler file cannot be located is skipped,
  instead of producing a DeviceNode that fails when DOS first tries to load it.
  Needs DOS and a Process, so pre-DOS (boot ROM) mounts are unchanged.
- **Non-boot CD mounting**:
  - data CDs mount via the `cdFS` recipe
  - Amiga-bootable CDs ("AMIGA BOOT"/"CDTV") get boot priority
  - RDB-formatted CDs still work
  - `CheckPVD()` reports four states (`PVD_ERROR`/`PVD_NONE`/`PVD_DATA`/
    `PVD_AMIGABOOT`) rather than folding "sector 16 unreadable" and "no ISO
    PVD" into one `-1`. Its only job now is boot priority; identifying the
    disc's format is the handler's (see `MSF_CD_ANYFMT`)
  - `ClassifyCD()` requires `scsi_Status == 0` as well as a clean `io_Error`
    before trusting the TOC buffer, so a CHECK CONDITION on the last retry no
    longer parses zeros
- **Device-name collision handling**: names get a trailing digit ensured
  ("UMSD" → "UMSD0") and bumped past collisions ("UMSD1" … "UMSD10"), checked
  against both the pre-boot MountList and the live DOS lists, so hotplugging a
  second stick can't reuse a name. Applies to RDB partition names too.
- **`MOUNTER_LOG` diagnostics hook**: define it and provide
  `mounter_log(fmt, ...)` to receive all mounter output (format strings are
  `%l`-normalized, safe for exec RawDoFmt sinks).
- **`MOUNTER_TRACE` verbosity knob** (now documented; was already wired up):
  define `MOUNTER_TRACE=1` on top of `MOUNTER_LOG` for verbose per-step
  tracing, layered on the same sink.

### Changed

- **`MountDrive()` return value**: total partitions mounted across all units
  - `> 0`: mounted at least one partition
  - `0`: media recognized but nothing mounted
  - `-1`: nothing recognized
  - Previously: MBR/GPT/CD mounts were never counted and only last unit's status survived
- **Partition extents are exact**: legacy partitions map block-for-block to
  `de_LowCyl`/`de_HighCyl` (1 block = 1 "cylinder"). The old CHS fitting
  rounded unaligned partitions (e.g. classic LBA-63 MBRs) one block off —
  data corruption on write.
- **MBR/GPT parsing overhauled**:
  - extended containers 0x0F/0x85 accepted; EBR chain walked with correct
    (container-relative) links
  - implausible tables rejected (`SaneMBR`)
  - GPT gated by protective MBR entry and validated header (position, size, CRC32)
  - honors `size_of_entry`, caps at 128 entries, skips partitions beyond 2^32
    blocks instead of truncating
- **Boot-node rule unified**: legacy/CD paths follow the RDB rule — a
  `ConfigDev` is passed only pre-DOS for bootable partitions, NULL otherwise.
  Both paths now go through one `AddMountNode()` (see Removed).
- **Reads past 4 GB use `TD_READ64`** (32-bit `CMD_READ` offsets stay for
  smaller disks; not every device implements TD64).
- **Sector sizes 256–4096 supported** (buffers sized for 4096); anything else
  is rejected per unit instead of overflowing.
- **Hardened against corrupt/hostile media**:
  - RDB environment vector, drive-name length byte, hunk counts/sizes and reloc
    offsets clamped (prevented heap/stack overflows)
  - PART/FSHD chains cycle-capped
  - failed filesystem load no longer leaves dangling FileSystem.resource entry

### Fixed

- Multi-unit scans no longer leak one unit's mount count into the next
- `ScanCDROM`/`register_legacy` `de_TableSize`: now 16 (up to `DE_DOSTYPE`),
  19 with `de_Control` (was incorrectly 80 = `sizeof(struct DosEnvec)`)
- Motor-off after probing used NULL/stale request when geometry failed
- `FileSysEntry` freed with wrong size on load-failure path

### Removed

- **Pre-3.1 Kickstart support** (2026-08-20). The fork requires AmigaOS 3.1
  (V40) and up, and uses the V36+ OS API unconditionally. Upstream still
  supports KS 1.3; this fork's consumers cannot run below V40, so the legacy
  paths were untestable code sitting in a pre-DOS boot path. Gone with it:
  - the `W_CreateMsgPort`/`W_DeleteMsgPort`/`W_CreateIORequest`/
    `W_DeleteIORequest` shims and the private `W_NewList` — exec's own V36
    `CreateMsgPort()`/`DeleteMsgPort()`/`CreateIORequest()`/`DeleteIORequest()`
    are used instead. The four `W_*` declarations are out of `mounter.h`.
  - the `lib_Version >= 37` gate around `CacheClearU()` and its `cacheclear()`
    wrapper
  - the `lib_Version >= 36` gate on the DOS device/volume/assign name check
  - the KS 1.3 arm of `AddNode()` — hand-built `BootNode` + `Enqueue()`, and
    `AddDosNode()` + `DeviceProc()` standing in for `ADNF_STARTPROC`.
    `AddNode()`/`AddLegacyNode()` are now one `AddMountNode()` over
    `AddBootNode()`, which is what `AddDosNode()` is defined to be.
  - fabricating `FileSystem.resource` when absent — it is a Kickstart resident
    at priority 80 from V40 on, and a second one on `SysBase->ResourceList`
    would shadow the real one
  - `copymem()`, a hand-rolled byte copy carrying an a4091 boot-ROM link
    constraint; `CopyMem()` is used instead
  - `expansion.library`/`dos.library` are opened with version 40 (the dos open
    stays optional — failing it is how pre-DOS is detected)

  68000 compatibility is unaffected and stays: the odd-address `HUNK_RELOC32`
  branch and the byte-wise big-endian reassembly are a CPU floor, not an OS one.

- **`ScanAllUnits()` and the `luns` / `hostId` `MountStruct` fields**
  (2026-08-20): the blind scan of SCSI targets 0–7 × LUNs, with the Phase V
  wide-SCSI unit encoding. Unreachable in this fork — neither consumer is a SCSI
  host and both always pass a unit-number array. `unitNum` is now **required**;
  NULL returns -1.
- **`cdBoot` field**: replaced by `MSF_NO_CD` (gated all CD mounting, not just
  booting); callers must switch; field slot is now `flags`
- **`DISKLABELS` compile-time gate**: MBR/GPT/superfloppy support always built;
  use `MSF_NO_LEGACY` at runtime instead
- **`ndkcompat.h`**: gone; format strings use literal `%ld`/`%lu`/`%lx`
- **MS0–MS9/CD0–CD9 name probing**: replaced by collision handling above
- **Dead logging knobs**: `DEBUG_MOUNTER`/`USE_SERIAL_OUTPUT` (unreferenced
  anywhere), the unused `Trace` alias, and `TRACE_LSEG`/`dbg_lseg` (its "on"
  branch was permanently unreachable — the code force-`#undef`'d `TRACE_LSEG`
  right before checking it). `MOUNTER_LOG`/`MOUNTER_TRACE` are the only
  logging knobs now.
