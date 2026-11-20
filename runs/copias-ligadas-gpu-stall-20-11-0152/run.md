# Run copias-ligadas-gpu-stall-20-11-0152 — copies on: the access violation is gone, the stall is back

Folder: `runs/copias-ligadas-gpu-stall-20-11-0152/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 01:52 (local) |
| Commit | `b23f45e` |
| Package | `UnchartedNDCUWP_1.0.26324.436_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0` (colour/depth copies active) |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Phase 2.1: with the arena pin in place, turn the colour/depth copies back on and see whether the
magenta disappears.

## Timeline

| Time | Event |
| --- | --- |
| 01:51 | launch, title, menu with **Continue (Ambushed)**; prompts in Xbox glyphs (A / B / D-pad) |
| 01:51 | `cross` on Continue |
| 01:52 | app gone; the log ends in a GPU stall after 25 632 submits |

## Failures

| Kind | Detail |
| --- | --- |
| GPU stall | `GPU stall: submit #25632 scheduler … signals …=21682, waits` repeated to the end |
| Access violation | **none** — the pin holds; this is a different failure now |
| Device loss | not printed before the process ended |

## Telemetry

| Metric | Value |
| --- | --- |
| Menu FPS | 45.6 (21.93 ms) with copies active |
| Memory on the menu | 3 193 / 3 441 MiB |

## Reached gameplay?

- Verdict: **no** — Continue did not finish loading with the copies active.

## Visual symptoms

The menu renders with plausible colours; the magenta only appears in the 3D scene, which this run
never reached.

## Captures

None kept: the last frame is the dashboard.

## Conclusion

Turning the copies on replaces the access violation with the documented GPU stall. The pin did its
job — the AV is a solved problem — so the copy path has its own failure, at a specific submit, and
that is what the original documentation called "the transfer-path stall with copies enabled".

The A/B the repository already specified is the next step: keep the copies active and skip the
composite fragment shader (`BB_SKIP_SHADERS=0xb4fa3a82`). If the stall disappears and the colours are
correct, the composite draw is the copy-path trigger.

## Next single action

Run `BB_SKIP_COPY=0` + `BB_SKIP_SHADERS=0xb4fa3a82` on Continue and compare colours and stability.
