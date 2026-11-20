# Run fin-removido-3-fps-20-11-0139 — `Finish()` out of `GetArena`, 3.4 FPS

Folder: `runs/fin-removido-3-fps-20-11-0139/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 01:39 (local) |
| Commit | `b33f151` + the `Finish()` removal |
| Package | `UnchartedNDCUWP_1.0.26324.436_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | none; D-pad input through `pad.ps1` |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Measure the frame cost after removing the per-replacement `scheduler.Finish()` from `GetArena` (the
pin/retire path now owns synchronisation) and after switching the prompts to Xbox.

## Telemetry

| Metric | Before (426) | After (436) |
| --- | --- | --- |
| `FIN` | 62 calls, 511 ms | **0 calls, 0 ms** |
| Frame time | 438–502 ms | **292 ms** |
| FPS | 0.0–2.2 | **3.4** |
| `ARENA` | 62–63 | 66 |
| RAM current / peak | 4 341–4 466 / 4 449–4 623 MiB | 4 421 / 4 467 MiB |

## Reached gameplay?

- Verdict: **yes**, boat deck, tutorial prompt, ammo HUD.

## Visual symptoms

- Textures still magenta/cyan with black blocks (Phase 2).
- Prompts now read **"Press LT to aim, press RT to shoot"** — Xbox glyphs, and the doubled text is
  gone.
- No triangular hole anywhere.

## Failures

| Kind | Detail |
| --- | --- |
| Device loss | none |
| Assert | none |
| Access violation | none |

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-gameplay-fin0.png`](captures/01-gameplay-fin0.png) | gameplay | `FIN 0 0ms ARENA 66`, 3.4 FPS, Xbox prompts |

## Conclusion

The `Finish()` per arena replacement was real and expensive: removing it took 219 ms off a 511 ms
frame and removed every scheduler stall from the frame's critical path. `ARENA 66` shows the
replacements still happen — they are now cheap, but the churn itself is the next suspect.

What remains of the 292 ms is not `FIN`. The next measurement has to split the frame between CPU
recording, the arena rebuild churn (`ARENA 66` per frame) and GPU wait.

## Next single action

Instrument the frame: time spent in `GetArena`/arena rebuild per frame, time in draw recording, and
the GPU wait, so the 292 ms is attributed instead of guessed.
