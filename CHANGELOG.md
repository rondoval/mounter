# Changelog — poseidon-fixes fork

All changes are relative to the upstream a4091-software mounter (`main`).
`struct MountStruct` and `struct MountFS` both grew, but zero-filling the new
fields keeps the classic behavior; existing callers only need to drop `cdBoot`
(see Removed) and rebuild. Both structs are caller-allocated and every field is
read, so caller and mounter must always be built from the same header.

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
- **Boot-node rule unified**: legacy/CD paths follow the RDB rule —
  `AddBootNode()` only pre-DOS for bootable partitions, `AddDosNode()`
  otherwise.
- **Reads past 4 GB use `TD_READ64`** (32-bit `CMD_READ` offsets stay for
  smaller disks, keeping KS1.3-era devices working).
- **Sector sizes 256–4096 supported** (buffers sized for 4096); anything else
  is rejected per unit instead of overflowing.
- **Hardened against corrupt/hostile media**:
  - RDB environment vector, drive-name length byte, hunk counts/sizes and reloc
    offsets clamped (prevented heap/stack overflows)
  - PART/FSHD chains cycle-capped
  - failed filesystem load no longer leaves dangling FileSystem.resource entry

## Removed

- **`cdBoot` field**: replaced by `MSF_NO_CD` (gated all CD mounting, not just
  booting); callers must switch; field slot is now `pad` + `flags`
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
