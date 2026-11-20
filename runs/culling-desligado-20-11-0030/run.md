# Run culling-desligado-20-11-0030 — BB_NO_CULL test

Folder: `runs/culling-desligado-20-11-0030/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 00:30 (local) |
| Commit | `e56bf6d` + local changes |
| Package | `UnchartedNDCUWP_1.0.26324.320_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_NO_CULL=1` |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Decide between the two remaining explanations for the missing half of the quads: back-face culling
(winding) or geometry/material.

## Timeline

| Time | Event |
| --- | --- |
| 00:28 | launch and scripted navigation toward the movie |
| 00:29 | the eight captures show the **dashboard**: the app had already exited |

## Telemetry

| Metric | Value |
| --- | --- |
| Frames captured | 8, all of the dashboard |
| Measurement | none — the app was not running |

## Reached gameplay?

- Verdict: **no**

## Failures

| Kind | Detail |
| --- | --- |
| App exit | exited before the captures, cause not in this run's log |
| Device loss | not confirmed |

## Captures

None kept: every frame is the dashboard.

## Conclusion

The run is void: it answers nothing about culling. The automation is the limiting factor — each
scripted press is an HTTP upload, so the sequence is several seconds slower than the screen it is
trying to catch, and the app exited before the loading screen.

The hypothesis that remains, and the one the code supports, is RECT_LIST: with no tessellation the
port draws the guest's three-corner rectangle as a single triangle. That needs a measurement where
the coin is on screen, not another blind scripted run.

## Next single action

Add a RECT draw counter to the HUD (`QUAD x idx y R z`) and have the user read it on the coin
screen; `R > 0` confirms the rect path in one glance.
