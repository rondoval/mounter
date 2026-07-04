# Changelog — poseidon-fixes fork

All changes are relative to the upstream a4091-software mounter (`main`).
`struct MountStruct` grew, but zero-filling the new fields keeps the classic
behavior; existing callers only need to drop `cdBoot` (see Removed).

## New

- **Filesystem recipes** (`struct MountFS`; `fatFS`/`ntfsFS`/`cdFS` in
  `MountStruct`): the caller decides per filesystem family which dostype to
  mount with, an optional handler file (e.g. `"L:fat95"`), the preferred DOS
  device name, `de_Control`, buffers, MaxTransfer and handler stack size.
  NULL recipes = classic behavior (FAT via FileSystem.resource only; NTFS
  skipped; CD01/CDVD for CDs).
- **Handler loading without FileSystem.resource**: if a recipe's dostype is not
  registered, the DeviceNode gets `dn_Handler` = the recipe's handler file
  (`dn_GlobalVec = -1`), so DOS loads the filesystem on first access.
  Partitions whose recipe resolves to neither are skipped cleanly.
- **`flags` in `MountStruct`**: `MSF_NO_RDB`, `MSF_NO_LEGACY`, `MSF_NO_CD`
  gate the three scans at runtime; `MSF_LEGACY_FIRST_ONLY` mounts only the
  first MBR/GPT/superfloppy filesystem per unit; `MSF_NO_BOOT` suppresses
  pre-DOS boot nodes everywhere.
- **Explicit unit mounting (hotplug)**: `unitNum` now works as the header
  always advertised — NULL scans SCSI targets 0–7 (classic), a value < 0x100
  mounts that single unit, otherwise it points to a `{count, unit...}` array.
  Array entries are overwritten with per-unit results.
- **NTFS/exFAT awareness and superfloppy support**: partition boot sectors are
  sniffed (`DetectVBR`) — FAT mounts via `fatFS`, NTFS via `ntfsFS`, exFAT and
  unknown content are skipped (previously everything mounted as FAT).
  A filesystem at block 0 (superfloppy) mounts as a whole-disk device.
- **Non-boot CD mounting**: data CDs mount via the `cdFS` recipe; Amiga-bootable
  CDs ("AMIGA BOOT"/"CDTV") get boot priority; RDB-formatted CDs still work.
- **Device-name collision handling**: names get a trailing digit ensured
  ("UMSD" → "UMSD0") and bumped past collisions ("UMSD1" … "UMSD10"), checked
  against both the pre-boot MountList and the live DOS lists, so hotplugging a
  second stick can't reuse a name. Applies to RDB partition names too.
- **`MOUNTER_LOG` diagnostics hook**: define it and provide
  `mounter_log(fmt, ...)` to receive all mounter output (format strings are
  `%l`-normalized, safe for exec RawDoFmt sinks).

## Changed

- **`MountDrive()` return value**: total partitions mounted across all units
  (> 0), 0 = media recognized but nothing mounted, -1 = nothing recognized.
  Previously MBR/GPT/CD mounts were never counted and only the last unit's
  status survived.
- **Partition extents are exact**: legacy partitions map block-for-block to
  `de_LowCyl`/`de_HighCyl` (1 block = 1 "cylinder"). The old CHS fitting
  rounded unaligned partitions (e.g. classic LBA-63 MBRs) one block off —
  data corruption on write.
- **MBR/GPT parsing overhauled**: extended containers 0x0F/0x85 accepted and
  the EBR chain walked with correct (container-relative) links; implausible
  tables rejected (`SaneMBR`); GPT is gated by its protective MBR entry and a
  validated header (position, size, CRC32), honors `size_of_entry`, caps at
  128 entries, and skips partitions beyond 2^32 blocks instead of truncating.
- **Boot-node rule unified**: legacy/CD paths follow the RDB rule —
  `AddBootNode()` only pre-DOS for bootable partitions, `AddDosNode()`
  otherwise.
- **Reads past 4 GB use `TD_READ64`** (32-bit `CMD_READ` offsets stay for
  smaller disks, keeping KS1.3-era devices working).
- **Sector sizes 256–4096 supported** (buffers sized for 4096); anything else
  is rejected per unit instead of overflowing.
- **Hardened against corrupt/hostile media**: the RDB environment vector,
  drive-name length byte, hunk counts/sizes and reloc offsets are clamped
  (previously heap/stack overflows); PART/FSHD chains are cycle-capped;
  a failed filesystem load no longer leaves a dangling FileSystem.resource
  entry in use.

## Removed

- **`cdBoot` field** — replaced by `MSF_NO_CD` (despite its name it gated all
  CD mounting, not booting). Callers must switch; the field slot is now `pad` +
  `flags`.
- **`DISKLABELS` compile-time gate** — MBR/GPT/superfloppy support is always
  built; use `MSF_NO_LEGACY` at runtime instead.
- **`ndkcompat.h`** — gone; format strings use literal `%ld`/`%lu`/`%lx`.
- **MS0–MS9/CD0–CD9 name probing** — replaced by the collision handling above.

## Fixed

- Multi-unit scans no longer leak one unit's mount count into the next.
- `ScanCDROM`/`register_legacy` claimed `de_TableSize` = 80 longwords
  (`sizeof(struct DosEnvec)`); now 16 (up to `DE_DOSTYPE`), 19 with `de_Control`.
- Motor-off after probing used a NULL/stale request when geometry failed.
- `FileSysEntry` freed with the wrong size on the load-failure path.
