# Run copias-sem-staging-tambem-cai-20-11-1244 — staging is not the copy-path blocker

Folder: `runs/copias-sem-staging-tambem-cai-20-11-1244/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 12:44 (local) |
| Commit | `d124e39` |
| Package | `UnchartedNDCUWP_1.0.26324.1530_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0`, `BB_COPY_FORCE_STAGING=0` |
| Watch | `watch.ps1 -Interval 6 -MaxMinutes 3` |

## Intent

The UWP copy path forces the staging buffer even for same-format copies
(`BB_COPY_FORCE_STAGING` defaults on). Turning it off routes same-format copies through
`CopyImage`. If the stall lived in the staging buffer, this should change the outcome.

## Timeline (watch frames)

| Elapsed | Event |
| --- | --- |
| 0 s | Friend Stats dialog, `cross` |
| 7 s | black loading |
| 13 s | loading coin, whole, 13.6 FPS, RAM 3 821 MiB |
| 18 s | app exited |

## Reached gameplay?

- Verdict: **no** — same point as with staging on.

## Failures

| Kind | Detail |
| --- | --- |
| Process exit | 18 s after the load starts, identical to `BB_COPY_FORCE_STAGING` on |

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-moeda-antes-de-sair.png`](captures/01-moeda-antes-de-sair.png) | loading | coin whole, HUD at 13.6 FPS |

## Conclusion

The staging buffer is **not** the copy-path blocker: both variants die at the same second of the
same load. The failure is in the copy itself or in the transfer path shared by both, and it is the
gate in front of the correct colours.

What this run also confirms: the coin is whole in both variants, so the triangulation fix is stable
across configurations.

## Next single action

Instrument the copy path's completion: log which submit carries the copies and which fence/semaphore
the next wait blocks on, then compare with the submit the stall reports.
