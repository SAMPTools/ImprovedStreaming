# Improved Streaming  
https://www.mixmods.com.br/2022/04/improved-streaming/  
Formerly called Load Whole Map

## Building (PC / GTA:SA)

The mod is an ASI plugin built with Visual Studio for **32-bit** GTA:SA
(`gta_sa.exe` 1.0 US — the build SA-MP attaches to).

### Prerequisites

- **Visual Studio 2019 or newer** with the *Desktop development with C++*
  workload and a Windows 10 SDK. The project uses platform toolset **v142**.
- **plugin-sdk** ([DK22Pac/plugin-sdk](https://github.com/DK22Pac/plugin-sdk)) —
  clone and build it (its build produces `plugin.lib` under `output\lib`),
  then set an environment variable **`PLUGIN_SDK_DIR`** pointing at its root.
  The project pulls all its includes and libs from `$(PLUGIN_SDK_DIR)`.
- **injector** ([thelink2012/injector](https://github.com/thelink2012/injector)) —
  **not included in this repo** but required (the source does
  `#include "..\injector\assembly.hpp"`). Place it at **`PC/injector/`** so
  that relative path resolves (i.e. `PC/injector/assembly.hpp` exists).

### Build

1. Open `LoadWholeMap.sln` in Visual Studio.
2. Select configuration **`GTASA Release`**, platform **`Win32`** (must be
   x86 — `gta_sa.exe` is 32-bit; an x64 build will not load).
3. The output path (`OutDir`) is hard-coded to the original author's disk
   (`G:\GTA SA The Modded Edition\modloader\...`). Either change it in the
   project's *General → Output Directory* to your own SA `modloader\` folder,
   or just grab the built `.asi` from the configured output folder.
4. Build. The result is **`ImprovedStreaming.SA.asi`**.

### Install

- Put `ImprovedStreaming.SA.asi` in `modloader\` (if you use modloader) or in
  the GTA:SA root folder together with an ASI loader (e.g. Ultimate ASI
  Loader).
- Put `ImprovedStreaming.ini` (and any preset) next to it. Set `LogMode=0` (or
  higher) to get the diagnostic log — see below.

### Recommended companions

- **[SilentPatch](https://github.com/CookiePLMonster/SilentPatch)** — removes
  `FILE_FLAG_NO_BUFFERING` from IMG reads (faster streaming via the OS cache)
  and fixes a streaming deadlock. Recommended alongside this mod.
- **Open Limit Adjuster / a 4GB (LARGE_ADDRESS_AWARE) patch** — required if
  you raise the streaming memory limit; without it the process is capped at
  ~2 GB of address space. See `docs/streaming-memory-safety.md`.
- If you run **Project2DFX**, disable its `PreloadLODs` — it overlaps with
  this mod's (removed) LOD preload and the two collide.

## LogMode

`LogMode` in the `[Settings]` section of `ImprovedStreaming.ini` controls
diagnostic logging. Output is written to `ImprovedStreaming.log` next to the
mod. Use it to measure how long the preload takes and to tune the settings.

| Value | Meaning |
|-------|---------|
| `-1`  | **Off (default).** No log file, zero overhead. |
| `0`   | **Timing summaries.** Anim load time, build time (collect + dedup/sort), the queue-built line (model count, duplicates removed, `LoadEach`, streaming memory), and a final summary block. |
| `1`   | Everything in `0`, **plus the reaper**: one line per eviction pass with memory %, how many models were freed, and a marker when nothing is left to free. |
| `2`   | Everything in `1`, **plus per-model and per-batch detail** (each disk batch's blocking time and cursor progress). Very verbose — for deep debugging only. |

### Reading the final summary (LogMode ≥ 0)

```
========================================
Preload finished.
  total wall-clock : 8423 ms (8.423 s)   <- real time from preload start to finish
  models requested : 24011 / 24011       <- models loaded / total queued
  throughput       : 2850 models/s
  disk batches     : 750
  blocking in load : 8100 ms total, worst batch 240 ms
  drain spanned    : 1 frame(s) (AmortizeLoad=0)
  memory used/avail: 1847 / 2000 MB
========================================
```

- **total wall-clock** — how long the whole preload took. Real time, not the
  game clock (which is frozen during the load).
- **worst batch** — the longest single main-thread stall. This is the size of
  the biggest freeze/hitch the player feels.
- **blocking in load vs total wall-clock** — how much of the time was actual
  disk reading vs. overhead.
- **throughput (models/s)** — a comparable number to benchmark different
  settings or drives (HDD vs SSD) against each other.
- **drain spanned** — number of frames the load ran across. `1` means a single
  blocking burst; higher values mean `AmortizeLoad` spread it out.

### Spotting problems

- **Reaper (LogMode ≥ 1) firing constantly at high memory % with "nothing
  more to free"** means the streaming pool is saturated — the game's own
  eviction is about to cause load/unload stutter. Raise `StreamMemoryForced`
  or tighten the ranges.
- A large **worst batch** with `AmortizeLoad=0` is the multi-second startup
  freeze. Turning on `AmortizeLoad` trades it for many small per-frame
  hitches instead.
