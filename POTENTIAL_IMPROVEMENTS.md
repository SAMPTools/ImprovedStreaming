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

**Research verdict (2026-07): SKIP as framed — at most a trivial one-char
fix, no migration needed.** Unbiased web research revised the premise:

- The proposal's "every other value is off by one" claim is likely wrong. The
  `ReadInteger(...) - 1` is a deliberate 1-based→0-based conversion, so values
  ≥2 already map correctly (`2`→WORKERS_SF, …). The *only* real defect is the
  `> 0` gate, which drops the single value mapping to index 0 — so
  `POPCYCLE_PEDGROUP_WORKERS_LA` can never be ignored. A one-char change
  (`> 0` → `>= 0`, keeping the `-1`) fixes exactly that and changes no
  currently-working value, so **no config-version bump or migration is
  needed**. The migration concern only applies to the raw-index remap variant
  — don't take that route.
- Ped-group indexing confirmed as described: index 0 is `WORKERS_LA`;
  ~57–60 groups ending in the `POPCYCLE_TOTAL_NUM_PEDGROUPS` sentinel.
- Near-dead feature under SA-MP: the server controls peds and ambient GTA
  population is disabled, so excluding a popcycle ped group from a preload has
  little practical effect in 0.3DL. Low upside even for the minimal fix.
- Mandatory piece (the AV guard) is already applied.

Sources: [plugin-sdk CPopCycle.h](https://github.com/DK22Pac/plugin-sdk/blob/master/plugin_sa/game_sa/CPopCycle.h),
[gta-reversed population system](https://deepwiki.com/mrxenginner/gta-reversed/9.1-population-system),
[GTAMods Popcycle.dat](https://gtamods.com/wiki/Popcycle.dat).

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

**Research verdict (2026-07): SKIP now — but easier than this note claims
when revisited.** Unbiased web research found:

- The stated difficulty is overstated. `CAnimManager::AddAnimBlockRef` is a
  pure `usRefs++` counter, decoupled from load I/O — it pins a slot against
  eviction, it does not require the block's data to be resident. So the ref
  bookkeeping does *not* have to be threaded through the drain loop: keep the
  eager `AddAnimBlockRef` loop (cheap, no I/O; it just protects the queued
  slots), fold only the `RequestModel` calls into `preloadQueue`, and drop the
  eager `LoadAllRequestedModels`. That is a small, low-risk change.
- Small, bounded payoff: ~180 anim blocks (band 25575–25754) from one archive
  (~30 MB total) → a sub-second freeze, an order of magnitude smaller than the
  map-model preload `AmortizeLoad` exists for. Anim.img byte size unverified.
- Only helps the intersection of two niche, default-off flags
  (`AmortizeLoad=1` **and** `PreLoadAnims=1`).
- Premature while the base `AmortizeLoad` drain path is itself unverified
  in-game. Sequencing: prove the map-model drain first; revisit anims only if
  a user reports the `PreLoadAnims` startup hitch.

Sources: [jte/GTASA CAnimManager.cpp](https://github.com/jte/GTASA/blob/master/Engine/Animations/CAnimManager.cpp),
[mtasa-blue CAnimManagerSA.cpp](https://github.com/multitheftauto/mtasa-blue/blob/master/Client/game_sa/CAnimManagerSA.cpp),
[MTA GTA:SA Resource Streaming](https://wiki.multitheftauto.com/wiki/GTA:SA_Resource_Streaming).

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

**Research verdict (2026-07): SKIP the full proposal — the "error-prone"
instinct holds and the reward is smaller than stated.** Unbiased web research
found:

- The benefit is overstated. Model conversion (`ConvertBufferToObject` — RW
  stream parse, geometry/texture instantiation) runs on the **main thread in
  both paths**, inside `ProcessLoadingChannel`. Async removes only the
  disk-*wait* idle, not the CPU conversion cost — which is usually the larger
  hitch. "Disk work moves off main thread → near-zero hitch" is only half true.
- The residency concern is real. The bulk is requested `GAME_REQUIRED`, which
  is evictable by design; draining it over many frames while the player moves
  gives `MakeSpaceFor` a wide window to reclaim already-loaded models →
  weakened load-whole-map guarantee.
- Closing that hole by pinning everything `KEEP_IN_MEMORY`/`MISSION_REQUIRED`
  makes the reaper (`RemoveLeastUsedModel`) a no-op and removes the memory
  pressure valve — colliding with the `docs/streaming-memory-safety.md` model.
  The two safe options contradict each other.
- The technique is proven only at trivial scale (the CLEO `RequestModel` +
  wait idiom). Dumping thousands of IDs loses rate control — a full-map
  preload could take minutes and be starved by gameplay `PRIORITY_REQUEST`s.
- Low SilentPatch-overlap (it touches only the CdStream read path, not request
  semantics), but SilentPatch already made reads fast, so the disk-wait async
  would remove is already small on SSD — reward is thin exactly where it's safe.
- If anything, prototype a narrow HDD-only opt-in (`RequestModel` + yield to
  the engine's `Update()` instead of `LoadAllRequestedModels`) measured with
  `[mem-detect]` logging for `GAME_REQUIRED` eviction under fast movement; drop
  it if there's no clear win.

Sources: [gta-reversed streaming](https://deepwiki.com/mrxenginner/gta-reversed),
[MTA GTA:SA Resource Streaming](https://wiki.multitheftauto.com/wiki/GTA:SA_Resource_Streaming),
[GTAMods Resource Streaming](https://gtamods.com/wiki/Resource_Streaming),
[plugin-sdk CStreamingInfo.h](https://github.com/DK22Pac/plugin-sdk/blob/master/plugin_sa/game_sa/CStreamingInfo.h),
[CLEO Redux async](https://re.cleo.li/docs/en/async.html),
[SilentPatch CHANGELOG-SA](https://github.com/CookiePLMonster/SilentPatch/blob/dev/CHANGELOG-SA.md).

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
