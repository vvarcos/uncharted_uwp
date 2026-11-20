# Goal: verifiable gameplay on the console

This is the tracking document for the work after M5. Every claim here needs a run entry under
[`runs/`](runs/README.md); a document is not evidence.

## The goal

**Uncharted: Drake's Fortune runs as real gameplay on the Xbox in Developer Mode: stable frame
rate, correct picture, no lost device, inside the 5120 MiB app ceiling.**

"Real gameplay" means a level is loaded and simulated, driven by input — not the menu, not a movie,
not the loading screen.

## What is true today (2026-11-20, build 1.0.26324.400)

| Fact | Value | Source |
| --- | --- | --- |
| Package prepares the guest image on the console | yes, byte-identical to the PC pipeline | `runs/preparo-no-console-sem-pc-19-11-2338/` |
| Menus and movies render correctly | yes, characters included | same entry |
| **QUAD_LIST geometry** | **fixed** — both triangles tile the quad | `runs/moeda-com-renderer-novo-r0-20-11-0046/`, verified by the user on 400 |
| Gameplay frame rate | 2.5–4.9 FPS, `FIN` 21–68 calls at 243–1656 ms | user captures, 2026-11-20 |
| Gameplay colours | whole scene magenta/cyan, black blocks where textures belong | user captures |
| Level load | **fixed** — the arena pin removed the access violation; gameplay reached | `runs/gameplay-sem-crash-20-11-0133/` |
| Memory in gameplay | ~4.5 GiB of 5 120 MiB | user captures |
| Colour/depth copies | any real copy ends in a GPU timeout; **the trigger is the copied content**, not the copy command or its barriers | `runs/barreiras-sem-copia-jogo-roda-20-11-1515/` and the runs listed in `PLAN.md` |
| Gameplay with copies disabled | reached and held for 2.2 minutes, 24 captures, 3 FPS | same entry |

The 600-second runs in `HISTORY.md` were a menu at 60 Hz. They remain presentation evidence only.

## Acceptance criteria

A run closes the goal only when **all** of these hold in the same run, with the entry written:

- [ ] Continuous gameplay for **600 s**, confirmed by level progress or scripted input, not by a
      frame counter alone.
- [ ] Mean frame rate **≥ 30 FPS**, 95th percentile frame time **< 40 ms** (720p).
- [ ] Peak app memory **< 4 600 MiB**.
- [ ] No `DXGI_ERROR_DEVICE_HUNG`, no `VK_ERROR_DEVICE_LOST`, no assert, no access violation.
- [ ] Correct picture: characters present, materials in real colours, no missing geometry.
- [ ] Screenshots of at least two distinct gameplay scenes attached.

## Phases

### Phase 0 — Geometry of the quads (done)

`QUAD_LIST` emitted `{v1,v2,v0}` + `{v1,v3,v0}`, which leaves the band between the diagonals
uncovered. The loading coin, the Bluepoint fade and the gameplay composite all showed that hole.
Fixed to `{v1,v2,v3}` + `{v1,v3,v0}`; confirmed on the console.

- [x] Coin, Bluepoint logo and composite show no triangular hole.
- [x] `R 0` on the loading screen proves RECT_LIST is not involved.
- [x] The BufferCache index read was tried and reverted (it made the coin a thin slice).

### Phase 1 — The level-load crash (done)

The app died right after the coin: `M5 bound buffer destroyed: guest=0x1100000000 … size=252`,
then `M5 av-scan: addr=0x10 … found: none in self module`. Fixed by pinning an arena while a
descriptor write holds its handle (`Buffer::bound_refs`) and destroying only unpinned arenas from the
retire list.

- [x] No `bound buffer destroyed` and no `av-scan` on the level load (`runs/gameplay-sem-crash-20-11-0133/`).
- [x] The app stays alive in gameplay, on the boat deck.

### Phase 2 — Colours and textures

- [ ] Stop drawing magenta/cyan: the sampled texture is garbage, not a UV visualisation.
- [ ] Remove the black blocks where textures are missing.
- [ ] Prove the sampled image matches the asset, not the descriptor default.

### Phase 3 — Frame rate

- [x] Remove the per-replacement `scheduler.Finish()`: 62 calls / 511 ms per frame became 0 / 0 ms,
      and the frame went from ~0.5 s to 292 ms (3.4 FPS).
- [ ] Attribute the remaining 292 ms: CPU draw recording, the 66 arena rebuilds per frame, or GPU
      wait.
- [ ] Intermediate target: 10 FPS in gameplay.
- [ ] Target: 30 FPS mean, 95th percentile frame time under 40 ms.

### Phase 4 — Stability

- [ ] 600 s of gameplay with copies active (`BB_SKIP_COPY=0`), not as a diagnostic mode.
- [ ] Peak memory under 4 600 MiB with headroom.

### Phase 5 — Gate

- [ ] Every acceptance criterion met in one run.
- [ ] `README.md`, `HISTORY.md`, `KNOWN_ISSUES.md` and `MEMORY.md` updated from that entry.

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

1. **Measure first.** No change to the renderer without a run entry that shows the cost or the
   defect it targets.
2. **One run, one entry.** Every console run gets a folder under `runs/` before the next run starts.
3. **A Hy4 subagent on every step.** Each step runs with a `hy4-flash-free` subagent doing the part
   that does not need the console: reading logs, auditing a diff, checking a hypothesis against the
   sources, drafting the entry. The main agent executes, deploys and captures.
4. **Decide from the documents.** Run modes and switches are chosen from `TROUBLESHOOTING.md` and
   the code, not from memory.
5. **Ask only when the console cannot answer.** A question goes to the user only when it needs
   private context or a destructive choice.
6. **The gate is judged by one run**, with all criteria in the same entry.
7. **A change is tested only when the version changes**, and the **renderer hash** must match the
   build output: `package.ps1` refuses a stale layout for exactly this reason.
8. **Do not fight the crash for evidence.** The level-load AV kills the app about two seconds after
   the loading coin, which is shorter than one screenshot round trip. When a frame must be seen
   there, the user takes it or the app is left paused; scripted capture is for states that last.
