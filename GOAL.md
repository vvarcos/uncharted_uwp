# Goal: verifiable gameplay on the console

This is the tracking document for the work after M5. It replaces the milestone language that
treated a menu at 60 Hz as gameplay. Every claim here needs a run entry under [`runs/`](runs/README.md);
a document is not evidence.

## The goal

**Uncharted: Drake's Fortune runs as real gameplay on the Xbox in Developer Mode: stable frame
rate, correct picture, no lost device, inside the 5120 MiB app ceiling.**

"Real gameplay" means the guest is loading a level and simulating it, driven by input, not the
menu, not a movie, not a static scene the presenter keeps flipping.

## What is true today

| Fact | Value | Source |
| --- | --- | --- |
| Menu presentation | ~60 Hz (41 774 frames / ~696 s) | `HISTORY.md`, corrected in `runs/000-reconstrucao/run.md` |
| Real gameplay frame rate | **3–4 FPS** (~250–330 ms/frame) | user observation, boat scene `.1415` |
| Gameplay memory | 4 411 / 5 120 MiB | `docs/CONTINUE_LIFETIME.md` |
| Device loss | `DXGI_ERROR_DEVICE_HUNG` on the scene composite (`b4fa3a82`) | `docs/CONTINUE_LIFETIME.md` |
| Visual defects | characters missing, vertices exploding, black triangle, magenta/cyan materials | user observation |

The 600-second runs measured presentation, not gameplay. The gate is open.

## Acceptance criteria

A run counts as the goal met only when **all** of these hold in the same run, and the entry exists
under `runs/`:

- [ ] Continuous gameplay for **600 s**, confirmed by level progress or scripted input, not by a
      frame counter alone.
- [ ] Mean frame rate **≥ 30 FPS**, 95th percentile frame time **< 40 ms** (720p).
- [ ] Peak app memory **< 4 600 MiB**, i.e. at least 500 MiB of headroom under the ceiling.
- [ ] No `DXGI_ERROR_DEVICE_HUNG`, no `VK_ERROR_DEVICE_LOST`, no assert, no access violation.
- [ ] No missing characters, no exploding vertices, no black triangle, no wrong-colour materials
      visible in the captured frames.
- [ ] Screenshots of at least two distinct gameplay scenes attached to the run entry.

## Phases

### Phase 1 — Measure before changing (current)

The 3–4 FPS has never been attributed. Do not optimize on a guess.

- [ ] Per-frame counter and timing for `scheduler.Finish()` calls in
      `buffer_cache.cpp:1587` (arena replacement path).
- [ ] Per-frame count of arena window replacements and of `covers()` rebuilds.
- [ ] Per-frame count of QUAD_LIST draws and of expanded indices
      (`vk_rasterizer.cpp:3304-3322`).
- [ ] Draw census by primitive type, joined to the frame time.
- [ ] Baseline run entry: menu, then first gameplay frame, same package.

### Phase 2 — Frame rate

- [ ] Attribute the ~300 ms frame: CPU draw submission, driver sync, or GPU work.
- [ ] Intermediate target: **10 FPS** in gameplay.
- [ ] Target: **30 FPS** mean, 95th percentile frame time under 40 ms.

### Phase 3 — Picture

- [ ] Characters present in every gameplay frame; nothing vanishes on scene change.
- [ ] No vertex explosion: verify vertex buffers and the logical page table for the affected draw.
- [ ] Black triangle gone.
- [ ] Materials render with correct colours (no magenta/cyan).

### Phase 4 — Stability

- [ ] Composite draw (`f585649d` / `b4fa3a82`) no longer loses the device.
- [ ] Colour/depth copies run by default (`BB_SKIP_COPY=0`), not as a diagnostic mode.
- [ ] 600 s with copies active.

### Phase 5 — Gate

- [ ] Every acceptance criterion above met in one run.
- [ ] `README.md`, `HISTORY.md`, `KNOWN_ISSUES.md` and `MEMORY.md` updated from that run entry.

## Non-goals

- Uncharted 2 and 3 (M6) until the gate above is met.
- Upscalers, frame generation, temporal upscalers.
- Raising or working around the 5120 MiB ceiling.
- Anything that requires buffer device address, sparse residency or tessellation shaders.

## Tracking

| Where | What |
| --- | --- |
| [`runs/INDEX.md`](runs/INDEX.md) | one row per run: mode, gameplay, FPS, peak RAM, device loss |
| `runs/<folder>/run.md` | the full record of one run, with `captures/` beside it |
| this file | phases and acceptance criteria, updated only when a run entry supports it |

## Execution protocol

The work does not wait for instructions that the repository already answers. The order is fixed:

1. **Measure first.** No change to the renderer without a run entry that shows the cost or the
   defect it targets. The HUD counters (`FIN`, `ARENA`, `QUAD`) are the first measurement of every
   gameplay run.
2. **One run, one entry.** Every console run gets a folder under `runs/` before the next run starts.
   A run without an entry did not happen.
3. **A Hy4 subagent on every step.** Each step runs with a `hy4-flash-free` subagent doing the part
   that does not need the console: reading logs, auditing a diff, checking a hypothesis against the
   sources, drafting the entry. The main agent executes, deploys and captures. When a step has two
   independent questions, two subagents run in parallel.
4. **Decide from the documents.** Run modes and switches are chosen from `TROUBLESHOOTING.md` and
   the code, not from memory. The agent picks the documented mode that answers the current
   question and records why in the entry.
5. **Ask only when the console cannot answer.** A question goes to the user only when it needs
   private context (which save, which chapter, which account) or a destructive choice. Everything
   else is decided and written down.
6. **The gate is judged by one run**, with all acceptance criteria in the same entry and the
   screenshots attached.
7. **A change is tested only when the version changes.** Rebuild, then `package.ps1`, then install,
   and confirm the new version number in the install output. A package version that repeats between
   builds means the console is running the old binary — the 20-11 0004 coin run wasted a cycle that
   way (`runs/moeda-sem-o-fix-instalado-20-11-0004/`).
8. **The version is not enough: check the renderer hash.** Between 23:36 and 00:35 the layout copy of
   `bbgpu_dll.dll` did not refresh, so versions 303, 320 and 330 all shipped the same renderer and
   three "tests" measured nothing. `package.ps1` now refuses to package when the layout DLL differs
   from the build output; treat that error as the signal that the console would run the old code.
