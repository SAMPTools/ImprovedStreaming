# Delegated to SilentPatch — do NOT reimplement here

SilentPatch (CookiePLMonster) already fixes several GTA:SA streaming and
memory issues at the engine level. This mod treats SilentPatch as a
**companion/prerequisite** and deliberately does **not** reimplement any of
the below. If a future idea overlaps with this list, the answer is "install
SilentPatch", not "add it to ImprovedStreaming" — duplicating an engine-level
patch risks conflicting hooks and double-patching the same bytes.

Source: SilentPatch `CHANGELOG-SA.md` and `SilentPatchSA/SilentPatchSA.cpp`
(https://github.com/CookiePLMonster/SilentPatch). Verified against the repo.

## Streaming / IMG reading

- **`FILE_FLAG_NO_BUFFERING` removed from IMG reads.** The stock game opens
  IMG archives with `FILE_FLAG_NO_BUFFERING`, bypassing the OS file cache and
  slowing streaming. SilentPatch removes it → faster streaming, cached reads.
  *This is the single biggest disk-throughput win and it is not ours to make.*
  Our disk-position sort is complementary (ordering), not a substitute.

- **CdStream data-race + deadlock fix.** SilentPatch wraps the CdStream
  worker/sync in a critical section (`CdStreamSync`, `CdStreamThread`),
  fixing the streaming deadlock that black-screened interior enter/exit (the
  old "set CPU affinity to 1 core" workaround). Our force-preload issues many
  concurrent CdStream reads, so this fix directly protects our burst — but it
  is engine-wide and must stay in SilentPatch, not be re-created here.

- **Overlapped I/O / `GetOverlappedResult` correctness + `SetFilePointer`
  64-bit offset.** SilentPatch hardens the async read path and handles IMGs
  larger than 4 GB. Engine-level, leave to SilentPatch.

## Streaming memory / rendering

- **"Streaming memory bug" fix** (graphical artifacts from ped animations at
  high RAM usage). This is the classic garbled-model/animation corruption
  that appears when streaming memory is pushed high. SilentPatch fixes it.
  **Directly relevant to us:** our anim preload + a high streaming limit is
  exactly the scenario that triggers this bug, and SilentPatch is what makes
  that combination safe. We do **not** try to fix rendering artifacts here.

- **RC-vehicle / Supply-Lines LOD timing** and the **streamed-entity-list
  expansion** (world flicker when looking down at high draw distance). Both
  engine-level rendering/streaming fixes owned by SilentPatch.

## What stays OURS (not covered by SilentPatch)

For clarity on the boundary — SilentPatch does **not** do any of these, so
they remain this mod's job:

- Force-**preloading** selected model-ID ranges (SilentPatch never preloads).
- Setting / clamping the streaming memory limit `ms_memoryAvailable`
  (SilentPatch never touches it — see `docs/streaming-memory-safety.md`).
- The gradual-unload reaper (`RemoveLeastUsedModel` on an interval).
- Disk-position ordering of *our* preload queue.
- Address-space / SA-MP detection and the `MaxRAM` guard (proposed).

## Rule of thumb

If a proposed feature is about **how the engine reads IMGs, the CdStream
thread, overlapped I/O, or rendering artifacts from high memory** → it is
SilentPatch's territory; recommend SilentPatch instead of implementing it.
If it is about **what to preload, how much memory to allow, or when to
unload** → it is ours.
