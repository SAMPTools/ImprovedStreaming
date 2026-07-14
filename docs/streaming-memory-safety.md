# Concept: Streaming-Memory Safety (auto-detect + MaxRAM guard)

Status: proposed, not implemented. Unverified on a Windows x86 rig.

This doc proposes making the streaming-memory limit **self-limiting** so a
user cannot configure a value that crashes the game, and adding a **process-
RAM guard** that unloads resources before the process hits its address-space
ceiling. It makes the existing conservative cap automatic: clamp the value
**down** to a safe ceiling on constrained systems, and add a `MaxRAM` runtime
backstop.

**This is not about allowing *more* streaming memory.** The mod's author
(Junior_Djjr) deliberately caps at 2000 MB (not 2048) and discourages high
limits — a too-high pool starves the rest of the process (e.g. pause-menu
textures fail to load), and the streaming limit is *designed to be reached*
so unused resources get freed to make room for new ones. This design keeps
that philosophy: the safe ceiling is never raised above the current 2000 MB
default; the work is preventing values that are too *high* for a given
machine, not enabling higher ones.

## Background

`gta_sa.exe` is a 32-bit process. A 32-bit process can address at most:

- **~2 GB** by default, or
- **~4 GB** with the `LARGE_ADDRESS_AWARE` (LAA / "4GB patch") flag set and
  running on a 64-bit Windows.

SA-MP runs inside the same `gta_sa.exe` process, so it shares this ceiling.
This ceiling is the real constraint — **not** the amount of physical RAM in
the machine.

The streaming-memory limit (`CStreaming::ms_memoryAvailable`) is the budget
for the streamed-resource pool (models, textures). It is only *part* of the
process's address space; the rest is needed for code, RenderWare/D3D9,
SA-MP, and other allocations (roughly 1–1.5 GB). If the streaming pool is set
too high, two things go wrong: (a) the process runs out of address space and
crashes (an out-of-memory access violation), and (b) even below that, a pool
so large it consumes the process's headroom leaves no room for other
streamed loads (the author's example: pause-menu textures) when the pool
fills. Neither depends on how much physical RAM the machine has.

The `MAX_MB = 2000` cap (not 2048) is the author's deliberate headroom for
exactly reason (b). This design treats that 2000 as the fixed safe default
and only ever clamps **below** it — never above.

### SA-MP changes the picture (important for this project)

This fork targets SA-MP (Nova Legacy). SA-MP does **not** leave the streaming
limit at the vanilla value — it sets `ms_memoryAvailable` itself at startup,
by system-RAM tier. Stated by SA-MP Beta Tester "Matite" and confirmed
directly from the source thread (SA-MP forum tid=595677):

| System RAM | SA-MP sets streaming to |
|------------|-------------------------|
| ≥ 4 GB     | **1024 MB**             |
| 2–4 GB     | 512 MB                  |
| 1–2 GB     | 256 MB                  |
| < 1 GB     | 128 MB                  |

Two hard consequences:

1. **SA-MP's 1 GB is a *no-LAA* cap, not a hard ceiling.** SA-MP picks its
   tier value assuming a 2 GB address space and knows nothing about LAA, so it
   never exceeds 1 GB even on a 4 GB-RAM machine. The documented ~1 GB failure
   (past ~1 GB used, textures stop loading; players restart every ~10 min —
   SA-MP forum tid=595677, users Endamete/enbman) is really the 2 GB
   address-space wall being hit. **The thread's own fix for it is the
   LARGE_ADDRESS_AWARE patch** — i.e. LAA lifts the wall, and with LAA you
   *can* safely raise streaming above 1 GB. So:
   - **No LAA:** keep ~1024 MB (SA-MP's value is correct; going higher hits
     the wall and is actively harmful).
   - **With LAA:** the SA-MP 1 GB auto-pick can be safely overridden upward to
     the address-space-safe value (~2000 MB; Junior's stated ceiling is ~3 GB,
     never 4). Raising it is the documented cure for the texture-stall.

   Caveat (the GC-trigger point below): raising the limit only helps if you
   are actually hitting the ~1 GB wall (heavy retextures). It does not improve
   base fidelity — the limit is a garbage-collection trigger, and RoSA needs
   only ~256–512 MB. Don't raise it speculatively.

   *Sourcing:* the tier table and the ~1 GB failure are community-sourced
   (SA-MP forum, cross-checked against the primary thread) — not official
   SA-MP documentation and not yet confirmed by a runtime read on this
   target. Reading `0x8A5A80` in-game (step below) is the authoritative
   check and should be done before the auto-clamp acts on these numbers.

   **Version caveat — this fork targets 0.3DL, the forum data is 0.3.7.**
   0.3DL is the renamed 0.3.8 RC, derived from 0.3.7 with server-side custom
   model downloading added. It *probably* inherits the same RAM-tier
   streaming allocation, but this is **unconfirmed** — no 0.3DL-specific
   streaming-memory figures were found. Two implications: (a) do not trust the
   0.3.7 tier table as fact for 0.3DL — the runtime `0x8A5A80` read is the
   only version-agnostic ground truth and becomes the primary input, with the
   table as a rough expectation only; (b) 0.3DL streams *extra* downloaded
   custom models (DFF/TXD) on top of the base map, so pool pressure is if
   anything *higher* than vanilla 0.3.7 — another reason the guard (part 3)
   and reading the real value matter more here, not less.
2. **`StreamMemoryForced` is re-asserted every tick**, so it overrides SA-MP's
   tier value. That is the exact mechanism that would push the pool past
   SA-MP's safe 1 GB and trigger the failure above. The mod must read and log
   SA-MP's value at `0x8A5A80` first, and must not silently exceed it.

Therefore the SA-MP safe ceiling is **address-space-dependent**: ~1024 MB
without LAA (do not exceed SA-MP's own value — the wall is real), and up to
the address-space-safe ~2000 MB with LAA (overriding SA-MP's LAA-unaware
1 GB auto-pick is safe there). The auto-clamp (below) is driven by the
detected address space, with SA-MP's 1 GB as the floor-of-safety only on a
non-LAA process.

Today the mod exposes `StreamMemoryForced` (a raw MB value, capped at
`MAX_MB = 2000`) and re-asserts it every tick. The upstream release also has a
`MaxRAM` option (unload when process RAM exceeds a value); **that is not
present in this codebase.** The two recurring user failures are:

1. Setting `StreamMemoryForced` high **without** the 4GB patch → crash at
   ~2 GB.
2. Driving through dense/HD-modded areas until the process approaches its
   ceiling → crash.

## Why

The current model pushes an address-space decision onto the user as a raw MB
number, with no knowledge of whether the 4GB patch is present. The
information needed to pick a safe value (the actual usable address space) is
available to the process at runtime — the mod should read it and clamp
itself, instead of trusting a hand-typed number. And a running-process RAM
backstop is the only thing that prevents a slow climb into the ceiling
during gameplay.

## Design

Three parts. All logging gates on the existing `LogMode`.

### 1. Detect the real address-space ceiling

At startup, query the usable ceiling with `GetSystemInfo` (or
`GetNativeSystemInfo`) and read `lpMaximumApplicationAddress`:

- ~`0x7FFEFFFF` (≈2 GB) → no LAA (or 32-bit OS).
- ~`0xFFFEFFFF` (≈4 GB) → LAA present on a 64-bit OS.

This is preferred over parsing the PE header's
`IMAGE_FILE_LARGE_ADDRESS_AWARE` flag because it reflects the *effective*
ceiling (LAA **and** the OS together), which is what actually matters.

Derive a safe streaming ceiling from it. The ceiling is **capped at the
existing default** and only reduced below it on a constrained system:

| Detected ceiling | Safe streaming cap |
|------------------|--------------------|
| ~2 GB (no LAA)   | ~1200 MB           |
| ~4 GB (LAA)      | 2000 MB (single-player default) |

**Under SA-MP without LAA, cap at ~1024 MB** — a larger pool hits the 2 GB
wall and breaks texture streaming. **Under SA-MP with LAA, the ~4 GB row
applies** (up to ~2000 MB) — LAA is exactly what fixes the multiplayer
texture-stall, so SA-MP's LAA-unaware 1 GB auto-pick may be safely exceeded.
So the effective cap is just the address-space-safe value; SA-MP only forces
the 1024 floor when LAA is absent. Detect SA-MP by the presence of `samp.dll`
and read the value it writes to `0x8A5A80` during init before overriding it.

The 4 GB row keeps the current default — it does **not** raise the pool.
Detecting LAA doesn't mean "use more"; it means a 2000 MB pool is safe there,
whereas on a ~2 GB ceiling even 2000 is unsafe and must be clamped down.
(Exact 2 GB-tier number to be tuned on a rig; ~1200 is a conservative start.)

### 2. Auto-clamp `StreamMemoryForced` (downward only)

Whatever the user sets (or whatever the preload's auto-increase would climb
to), clamp it to the safe cap from step 1. The user can always request
*less*; they can never push it *above* the safe cap (which itself never
exceeds the 2000 MB default). Log the detected ceiling, the chosen cap, and
whether a requested value was clamped down and why.

Effect: the "high value without the 4GB patch" crash becomes impossible — on
a ~2 GB ceiling the pool is forced down to a value the address space can
actually hold. `MAX_BYTE_LIMIT` and the auto-increase logic
(`IncreaseStreamingMemoryLimit`) all become bounded by this dynamic cap
instead of the static 2000.

### 3. `MaxRAM` process-RAM guard

Add an ini option `MaxRAM` (MB; default e.g. 3100 on a 4 GB ceiling, lower on
2 GB). In the steady-state reaper (the `loadCheck == 4` block), read the
actual process working set with `GetProcessMemoryInfo`
(`PROCESS_MEMORY_COUNTERS::WorkingSetSize`; requires `<psapi.h>` and linking
`psapi.lib`). If it exceeds `MaxRAM`, force `RemoveLeastUsedModel` to run —
independent of the existing `removeUnusedWhenPercent` gate — until the
process drops back under the limit.

This is the real backstop against the address-space wall: the streaming-pool
limit alone doesn't account for non-streaming allocations, but working-set
does.

## Tradeoffs

- **Auto-clamp removes user control above the safe cap.** Deliberate — that
  control is the footgun, and per the author's guidance high limits are
  undesirable anyway. Users can still tune *downward*. A "bypass the clamp"
  override is intentionally **not** offered: it would only let users re-create
  the crash, and going above 2000 contradicts the design intent.
- **The safe-cap numbers are heuristic.** They reserve a fixed headroom;
  a process with unusually heavy non-streaming mods could still need a lower
  value. `MaxRAM` (part 3) catches that case at runtime, so the static cap
  doesn't have to be perfect.
- **`psapi` dependency.** Small, standard Windows library; adds a link
  dependency to the project. Acceptable.
- **Reading working set every reaper tick** is a cheap syscall; the reaper is
  already rate-limited, so cost is negligible.

## Edge cases

- **32-bit OS** (no possibility of 4 GB): `lpMaximumApplicationAddress`
  reports ~2 GB → 2 GB cap path. Correct automatically.
- **`lpMaximumApplicationAddress` between the two values** (unusual `/3GB`
  boot configs): treat anything below ~3 GB as the 2 GB tier to stay safe.
- **`GetProcessMemoryInfo` fails**: skip the `MaxRAM` guard for that tick
  (fail open — never crash the guard itself), log once.
- **User sets `MaxRAM` above the ceiling**: clamp it to the detected ceiling
  minus headroom so it can still fire.
- **Percent-of-system-RAM values** (as LimitAdjuster's `MemoryAvailable`
  uses): explicitly **not** adopted — physical RAM is the wrong basis for a
  32-bit process. See "Out of scope".

## Out of scope

- **Percent-of-RAM configuration.** Physical RAM does not reflect the 2/4 GB
  process ceiling. If a percentage is ever wanted, it must be a percentage of
  the detected address-space ceiling, not of system RAM — but a fixed MB cap
  chosen by the auto-detect is clearer and is what this design uses.
- **Raising the cap above 2000 MB / a `DoubleStreamingMemoryLimit`-style
  option.** Explicitly not a goal. The author caps at 2000 on purpose (pool
  headroom for other loads; limits are meant to be reached so unused
  resources free up), and the recommended use is *selective* preloading
  (vehicles, props, anims for HD mods), not loading the whole map. This design
  only makes the existing conservative cap safer, never larger.
- **Applying the 4GB patch itself.** The mod detects LAA but does not patch
  it in; that stays the job of Open Limit Adjuster / f92la. (Self-patching
  LAA at runtime is possible but out of scope and risky.)
- **Coexistence with LimitAdjuster's `MemoryAvailable`.** Documented
  separately: pick one owner of the streaming limit. This mod's per-tick
  re-assert wins regardless, so if this design ships, LimitAdjuster's
  `MemoryAvailable` should be left unset.

## Implementation order

1. Address-space detection helper (`GetSystemInfo` →
   `lpMaximumApplicationAddress` → safe cap). Log-only first, to verify the
   detected numbers on a rig before anything acts on them.
2. Auto-clamp `StreamMemoryForced` and the auto-increase logic to the dynamic
   cap (which is `min(detected-safe, 2000)` — never above 2000).
3. `MaxRAM` guard in the reaper (`GetProcessMemoryInfo` + forced unload).
4. README: document the new automatic behavior and `MaxRAM`; note that
   `StreamMemoryForced` above the auto cap is clamped down, and that there is
   deliberately no option to go above 2000 MB.

Each step is independently shippable and independently verifiable in
`ImprovedStreaming.log`.
