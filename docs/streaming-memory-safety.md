# Concept: Streaming-Memory Safety (auto-detect + MaxRAM guard)

Status: proposed, not implemented. Unverified on a Windows x86 rig.

This doc proposes making the streaming-memory limit **self-limiting** so a
user cannot configure a value that crashes the game, and adding a **process-
RAM guard** that unloads resources before the process hits its address-space
ceiling. It replaces the manual, error-prone `StreamMemoryForced` /
`DoubleStreamingMemoryLimit` tuning with an automatic safe ceiling, and adds
a `MaxRAM` runtime backstop.

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
too high for the available address space, the process runs out of address
space and crashes (an out-of-memory access violation), regardless of how
much physical RAM exists.

Today the mod exposes `StreamMemoryForced` (a raw MB value, capped at
`MAX_MB = 2000`) and re-asserts it every tick. The upstream release also has
`DoubleStreamingMemoryLimit` (raises the cap toward 4096) and `MaxRAM`
(unload when process RAM exceeds a value); **neither is present in this
codebase.** The two recurring user failures are:

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

Derive a safe streaming ceiling from it, reserving headroom for everything
else in the process:

| Detected ceiling | Safe streaming cap |
|------------------|--------------------|
| ~2 GB            | ~1200 MB           |
| ~4 GB            | ~2500 MB           |

(Exact numbers to be tuned on a rig; these are conservative starting points
consistent with the community "2.4 GB RoSA is stable" data point.)

### 2. Auto-clamp `StreamMemoryForced`

Whatever the user sets (or whatever the preload's auto-increase would climb
to), clamp it to the safe cap from step 1. The user can request *less* than
the cap, never more. Log the detected ceiling, the chosen cap, and whether a
requested value was clamped down and why.

Effect: the "high value without the 4GB patch" crash becomes impossible — the
mod refuses to exceed what the address space can hold. `MAX_MB` /
`MAX_BYTE_LIMIT` and the auto-increase logic
(`IncreaseStreamingMemoryLimit`) all become bounded by this dynamic cap
rather than the static `2000`.

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
  control is the footgun. Users can still tune *downward*. A power user who
  truly wants more can be given an explicit `IKnowWhatImDoing`-style override
  that bypasses the clamp, but the default must be the safe automatic value.
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
   cap. Replaces the static `MAX_MB = 2000` ceiling.
3. `MaxRAM` guard in the reaper (`GetProcessMemoryInfo` + forced unload).
4. README: document the new automatic behavior and `MaxRAM`; note that
   `StreamMemoryForced` above the auto cap is now clamped, and that
   `DoubleStreamingMemoryLimit` is unnecessary because the cap is dynamic.

Each step is independently shippable and independently verifiable in
`ImprovedStreaming.log`.
