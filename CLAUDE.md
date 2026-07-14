# CLAUDE.md

Context and rules for working on **ImprovedStreaming** (formerly "Load Whole
Map"), a GTA:SA ASI mod that force-preloads map models to reduce streaming
stutter and map holes. This fork targets **SA-MP 0.3DL** (Nova Legacy).

## Read these first

Before proposing or changing anything streaming/memory-related, read:

- [docs/streaming-memory-safety.md](docs/streaming-memory-safety.md) — the
  proposed memory-limit auto-detect + clamp + `MaxRAM` design, and *why the
  streaming limit is a GC trigger, not a memory target*. Also the SA-MP 1 GB
  ceiling and the 0.3DL-vs-0.3.7 uncertainty.
- [docs/delegated-to-silentpatch.md](docs/delegated-to-silentpatch.md) —
  what SilentPatch already fixes at the engine level. **Do not reimplement
  anything on that list.**
- [POTENTIAL_IMPROVEMENTS.md](POTENTIAL_IMPROVEMENTS.md) — reviewed-but-
  deferred changes and research findings, with rationale/risk each.
- [README.md](README.md) — build steps and companion mods.

## Hard rules

- **Untested on a real rig.** Everything here was authored on macOS, where the
  plugin-sdk headers cannot compile — clang diagnostics about missing
  `plugin.h` etc. are expected noise, not real errors. Nothing has been built
  or run in-game. Never claim a change "works"; say it is unverified and needs
  a Windows x86 build + one in-game check. Flag this in every hand-off.
- **x86 only.** `gta_sa.exe` is 32-bit; the ASI must be built Win32. Targets
  1.0 US (`PLUGIN_SGV_10US`) — the build SA-MP attaches to.
- **Don't reimplement SilentPatch.** Engine read path (CdStream, IMG buffering,
  overlapped I/O), and rendering artifacts from high memory, are SilentPatch's
  job. See the delegation doc. Recommend it as a companion instead.
- **Memory limit: never raise above the conservative default.** The author
  (Junior_Djjr) caps at 2000 MB on purpose; under SA-MP the safe value is
  ~1024 MB without LAA (up to ~2000 with LAA). More memory does NOT improve
  fidelity — the limit is a GC trigger. Work is making the cap *safer*, never
  *larger*. See the memory-safety doc.
- **Verify addresses/field names against the pinned plugin-sdk**, not from
  memory. `m_nImgId`, `POPCYCLE_TOTAL_NUM_PEDGROUPS`, RVAs like `0x8A5A80`
  have been cross-checked once; re-check if you touch them.

## Architecture (the one file that matters)

Nearly all logic is in [PC/LoadWholeMap/LoadWholeMap.cpp](PC/LoadWholeMap/LoadWholeMap.cpp),
built on the `plugin` (DK22Pac plugin-sdk) event system + the `injector` lib.

- Runs on the **game main thread** via `Events::processScriptsEvent.after`.
  Anything expensive there is a frame hitch.
- `loadCheck` state machine: `<3` skip initial ticks; `==3` build the preload
  queue (once) then drain it; `==4` steady-state reaper.
- **Preload:** collect enabled `[RangeN]` model IDs from the ini → dedup →
  sort by on-disk position `(m_nImgId, m_nCdPosn)` → `RequestModel` in
  `LoadEach`-sized batches → `LoadAllRequestedModels(false)` (blocking).
  `AmortizeLoad=1` spreads the drain across frames instead of one freeze.
- **Reaper:** when memory% ≥ `RemoveUnusedWhenPercent`, evict a small batch via
  `RemoveLeastUsedModel` on an interval. The real anti-thrash mechanism is the
  large pool, not the reaper.
- **Logging:** `LogMode` in the ini (`-1` off … `2` verbose). `[mem-detect]`
  lines (step 1 of the memory-safety doc) log the address space + SA-MP value
  — this is how we get 0.3DL ground truth.

## Building

See README "Building". Short version: VS2019+ (toolset v142), `GTASA
Release|Win32`, needs `PLUGIN_SDK_DIR` env var (built plugin-sdk) and the
`injector` lib placed at `PC/injector/` (not committed). Output
`ImprovedStreaming.SA.asi`.

## Git

This is the `SAMPTools/ImprovedStreaming` fork of `JuniorDjjr/LoadWholeMap`.
Commit with clear messages; push to `main` only when asked. Keep commit bodies
honest about the untested status.
