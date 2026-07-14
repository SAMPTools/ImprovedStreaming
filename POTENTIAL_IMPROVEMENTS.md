# Potential Improvements

Reviewed-but-deferred changes to `PC/LoadWholeMap/LoadWholeMap.cpp`. These
were identified during a code review of the reworked preload path but
intentionally **not** applied, because each carries a behavior-change or
design risk that should be validated on a real Windows x86 rig before
shipping. The safe, high-confidence findings from the same review are
already committed.

Line numbers are approximate and drift as the file changes — anchor by the
surrounding code, not the number.

---

## 1. `IgnorePedGroup` off-by-one

**What it fixes.** In the `.ini`, `IgnorePedGroup` is 1-based, but the code
does `ReadInteger(...) - 1` and then gates on `ignorePedGroup > 0`. Two
consequences:

- The first ped group (0-based index `0`,
  `POPCYCLE_PEDGROUP_WORKERS_LA`) can never be selected — `IgnorePedGroup=1`
  maps to index `0`, which the `> 0` gate then skips.
- Every other value is off by one relative to what the user thinks they
  picked (`IgnorePedGroup=2` actually targets group index `1`).

A fix would make the config number line up with the actual ped group.

**Why deferred.** Existing shipped `.ini` presets are tuned against the
current (skewed) mapping. Correcting the offset shifts every user's group
selection by one, so configs that work today would suddenly ignore the
wrong group — a silent behavioral break. Only the **crash guard** (an upper
bound on the index, so an out-of-range value can't AV the host via
`CPopCycle::IsPedInGroup`) was mandatory and has already been applied.

**Risk.** Low technical risk, but a semantics change that breaks existing
configs. Do it only alongside a config-version bump or a documented
migration note.

**Sketch.** Decide on 0-based vs 1-based, apply consistently, and keep the
existing upper-bound guard:

```cpp
// if adopting a clean 0-based mapping:
if (ignorePedGroup >= 0 && ignorePedGroup < POPCYCLE_TOTAL_NUM_PEDGROUPS
    && CPopCycle::IsPedInGroup(model, ignorePedGroup)) { ... }
```

---

## 2. Amortize the animation preload

**What it fixes.** `AmortizeLoad=1` exists to eliminate the multi-second
startup freeze by spreading the preload across many frames. It currently
only amortizes the **map model** ranges. When `PreLoadAnims=1`, the
animation blocks are still requested and loaded in a single blocking
`LoadAllRequestedModels(false)` call in the build phase — so a user who
enabled `AmortizeLoad` specifically to avoid freezes still eats one up
front, proportional to the anim-block count.

A fix would make `AmortizeLoad=1` genuinely freeze-free even with
`PreLoadAnims=1`.

**Why deferred.** `AmortizeLoad` is itself experimental, default-off, and
unverified on a rig. Folding the anim blocks into the shared `preloadQueue`
so they drain through the same per-frame batch path is fiddly:
`CAnimManager::AddAnimBlockRef` still has to be called per block, so the
ref-count bookkeeping has to be threaded through the drain loop rather than
done eagerly. Not worth the complexity until the flag itself is proven
in-game.

**Risk.** Safe in principle (same models, same flags, only load timing
changes), but touches anim ref-counting — verify anims aren't needed before
the first frame they would otherwise stream in.

**Sketch.** Push `id + animBlocksIdStart` into `preloadQueue` with
`MISSION_REQUIRED` instead of the dedicated eager load; call
`AddAnimBlockRef` as each anim model is drained (or keep the ref loop but
drop the eager `LoadAllRequestedModels`/`CTimer` pair).

---

## 3. True async streaming for amortized mode

**What it fixes.** Even in amortized mode, each per-frame batch calls the
**blocking** `CStreaming::LoadAllRequestedModels(false)`, which stalls the
main thread until that batch finishes reading from disk. So `AmortizeLoad`
turns one long freeze into N smaller per-frame hitches — better, but not
hitch-free.

The genuinely smooth alternative: `RequestModel` the batch and let the
game's own per-frame non-blocking streamer (`LoadRequestedModels`, serviced
by the `CdStream` worker thread) pull models in over subsequent frames. Disk
work moves off the main thread → near-zero per-frame hitch during preload.

**Why deferred.** This is a design change, not a patch:

- `GAME_REQUIRED` models can be evicted before the queue finishes if the
  pool fills, so the "load whole map" guarantee weakens unless everything is
  requested `MISSION_REQUIRED`/`KeepLoaded`.
- The tight `CTimer::Stop()`/`Update()` accounting no longer applies —
  there's no discrete blocking window to hide.

**Risk.** High. Changes residency/eviction semantics and the timing model.
Keep the current blocking-batch path as the default; expose async as an
opt-in mode and validate memory behavior under fast movement.

---

## Related, lower-value notes (not tracked as improvements)

These came up in review but are minor and not scheduled:

- **Latent `IniReader` throws** — `ReadString` and `ends_with` have
  edge-case exception/UB paths on malformed input, but neither is called on
  the active code path today.
- **`::tolower` on a possibly-negative `char`** in `IniReader` (`starts_with`
  for the `0x` prefix check) is technically UB for high-bit (cp1252) bytes;
  cast to `unsigned char` if touched.
- **Build-phase micro-costs** — the two `std::sort` passes and the
  array-deref comparator are one-time CPU (single-digit ms for ~20k
  entries), no disk-throughput effect. A packed `(imgId << 32) | cdPosn`
  sort key and insertion-time dedup would tidy it but change nothing
  measurable.
- **`RemoveUnusedInterval` not lower-bounded** — `0` or negative makes the
  reaper run every frame; a churn/perf concern, not a crash.

---

## From web research (GTA SA / SA-MP streaming)

Findings from researching community streaming knowledge. Sources: MTA Wiki
(Resource Streaming), GTAMods Wiki, SA-MP forum, SilentPatch notes,
DK22Pac/plugin-sdk, mixmods.

- **Preload with a keep-in-memory flag, or it's wasted.** `RequestModel(id,
  GAME_REQUIRED)` loads a model the engine's own reaper may evict the next
  frame; only `MISSION_REQUIRED` (our `KeepLoaded=1`) pins it. With the
  default `KeepLoaded=0`, a chunk of the preload can be reaped immediately,
  and our reaper + the engine's reaper can thrash. **Consider defaulting
  preloaded ranges to a keep flag**, or at least documenting that `KeepLoaded`
  should usually be `1`. Verify the exact R5 flag values on target.
- **The disk-position sort is lower-value than assumed.** The engine already
  issues streaming reads in IMG-offset order via `GetNextFileOnCd`
  (`0x408E20`). Our pre-sort is correct in spirit but the win is ~0 on SSD
  and marginal on HDD. Keep it (it's cheap), but don't invest further; it is
  not the throughput lever it looks like.
- **SilentPatch is the real disk-throughput win — as a companion, not a
  reimplementation.** It removes `FILE_FLAG_NO_BUFFERING` from IMG reads (lets
  the OS cache them) and fixes a streaming deadlock. Recommend it as a
  prerequisite in the README rather than duplicating its behavior.
- **The `LoadAllRequestedModels(false)` burst is the freeze** — corroborated
  by community reports of multi-second stalls when the pool flushes a large
  amount at once. This is what `AmortizeLoad` addresses; the research
  supports prioritizing that path.
- **Model-ID bands (reference for targeting preload ranges):** DFF 0–19999,
  TXD 20000–24999, COL 25000–25255, IPL 25256–25510, DAT 25511–25574,
  IFP anims 25575–25754, RRR 25755–25819. Preloading all anims (the
  SAM-P-relevant case) = the 25575–25754 band.
- **`MakeSpaceFor` (`0x40E120`) / `RemoveLeastUsedModel` (`0x40CFD0`) respect
  streaming flags.** Our reaper already calls the real `RemoveLeastUsedModel`,
  so it inherits the engine's "don't evict in-use/flagged" safety — good. Do
  not replace it with a hand-rolled LRU eviction, which could free an in-use
  model and AV.

The SA-MP streaming-memory tiering and the ~1 GB SA-MP ceiling are covered in
detail in [docs/streaming-memory-safety.md](docs/streaming-memory-safety.md).
