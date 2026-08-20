# Changelog — poseidon-fixes fork

**This fork no longer tracks upstream.** It began as the a4091-software mounter
(`main`) and stayed mergeable with it for a long while, but the 2026-08 restructure
below reorganised the module past the point where upstream changes can be merged.
Fixes flow one way now: anything worth sending back has to be ported by hand. The
sections after "2026-08" still describe the divergence from upstream, and are kept
because they explain *why* the code looks the way it does — but they are history,
not a diff you can apply.

**The fork requires AmigaOS 3.1 (V40) and up** — upstream's Kickstart 1.3/2.x
fallbacks are gone (see Removed).

## 2026-08 — restructure

One mount path, four files, one naming convention. Behaviour is unchanged except
where noted; every parse, clamp and retry count is exactly as it was.

- **One node-creating site.** Three functions used to build a `DeviceNode`, each
  with its own copy of: duplicate-extent check, DOS naming, `MakeDosNode()`,
  filesystem attach, `AddBootNode()`. The scanners are now pure enumerators —
  each fills a `struct Volume` and calls `mnt_mount_volume()`, which owns that
  whole tail. `mount_recipe()` is gone.
- **Fixed: `renamed` was inflated by the case it exists to detect.** The CD and
  MBR/GPT paths fixed the DOS name *before* the duplicate-extent check, so a
  second pass over an already-mounted volume bumped the name (and the counter)
  before deciding not to mount anything. The unified path checks the extent first.
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

### API, this release

Breaking. Both consumers were updated in the same change.

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

---

The sections below predate the restructure and describe the divergence from
upstream a4091-software.

## New

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

## Changed

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

## Removed

- **Pre-3.1 Kickstart support. The fork now requires AmigaOS 3.1 (V40) and up**,
  and uses the V36+ OS API unconditionally. Upstream still supports KS 1.3; this
  fork's consumers cannot run below V40, so the legacy paths were untestable
  code sitting in a pre-DOS boot path. Gone with it:
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

- **`ScanAllUnits()` and the `luns` / `hostId` `MountStruct` fields**: the blind
  scan of SCSI targets 0–7 × LUNs, with the Phase V wide-SCSI unit encoding.
  Unreachable in this fork — neither consumer is a SCSI host and both always
  pass a unit-number array. `unitNum` is now **required**; NULL returns -1.
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

## Fixed

- Multi-unit scans no longer leak one unit's mount count into the next
- `ScanCDROM`/`register_legacy` `de_TableSize`: now 16 (up to `DE_DOSTYPE`),
  19 with `de_Control` (was incorrectly 80 = `sizeof(struct DosEnvec)`)
- Motor-off after probing used NULL/stale request when geometry failed
- `FileSysEntry` freed with wrong size on load-failure path
