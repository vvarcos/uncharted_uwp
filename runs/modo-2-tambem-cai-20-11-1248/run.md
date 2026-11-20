# Run modo-2-tambem-cai-20-11-1248 — same-format copies alone also end the load

Folder: `runs/modo-2-tambem-cai-20-11-1248/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 12:48 (local) |
| Commit | `4628452` |
| Package | `UnchartedNDCUWP_1.0.26324.1530_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=2` (depth↔colour copies skipped, same-format copies active) |
| Watch | `watch.ps1 -Interval 6 -MaxMinutes 3` |

## Intent

Narrow the copy-path failure: if only the same-format copies are active and the load survives, the
depth↔colour transfer is the culprit and the unused render backend could replace just that.

## Timeline (watch frames)

| Elapsed | Event |
| --- | --- |
| 0 s | Friend Stats dialog, `cross` |
| 7 s | black loading |
| 13 s | loading coin, 13.6 FPS, 68 KiB frame |
| 18 s | app exited |

## Reached gameplay?

- Verdict: **no** — same second as every other copy configuration.

## Failures

| Kind | Detail |
| --- | --- |
| Process exit | 18 s after the load starts, identical to modes 0, 1-off and staging-off |

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-moeda-antes-de-sair.png`](captures/01-moeda-antes-de-sair.png) | loading | coin whole, HUD |

## Conclusion

Both copy classes fail at the same point, so the transfer path itself is the problem — not a
format, not the staging buffer, not one direction of the conversion.

That changes the shape of the fix: `gpu/shim/uwp_depth_copy.cpp` (commit `50f8603`) is a complete
render-based copy backend with its own pipeline and shader, and **nothing calls it**. It was written
as opt-in and never wired. It now has to become the copy path, replacing the transfers.

## Next single action

Wire `UwpDepthCopy` into `Runtime::CopyColorAndDepth` behind `BB_DEPTH_COPY_RENDER=1`, using the
scheduler's command buffer, and compare against this run.
