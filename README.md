# Improved Streaming  
https://www.mixmods.com.br/2022/04/improved-streaming/  
Formerly called Load Whole Map

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
