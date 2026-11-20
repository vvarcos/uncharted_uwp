# Run continue-com-copias-moeda-e-saida-20-11-1209 — copies on: the coin is whole, then the app leaves

Folder: `runs/continue-com-copias-moeda-e-saida-20-11-1209/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 12:09 (local) |
| Commit | `e1845c1` |
| Package | `UnchartedNDCUWP_1.0.26324.436_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0` (copies active) |
| Watch | `watch.ps1 -Interval 5 -MaxMinutes 10` |

## Intent

Repeat the Continue path with copies on, but **watch the app until it exits** instead of sampling at
a fixed time, after the previous black frame turned out to be the loading gap.

## Timeline (from the watch frames)

| Elapsed | Event |
| --- | --- |
| 0 s | Friend Stats dialog (Continue asks again) |
| 0 s | `cross` on OK |
| 6 s | black loading |
| 11 s | **the loading coin, whole** — 13.1 FPS, 76.35 ms, RAM 3 698 MiB |
| 18 s | app exited (`watch: app-exited after 0.3 min, 3 frames`) |

## Reached gameplay?

- Verdict: **no** — the load dies right after the coin.

## Visual symptoms

- The coin is **complete**: no V notch. The triangulation fix is visible on the real loading screen,
  which is the confirmation Phase 0 was missing.
- No magenta in this frame (the scene never loads).

## Failures

| Kind | Detail |
| --- | --- |
| Process exit | 18 s after the OK, during the level load |
| Cause | the copy path's own failure (documented as the transfer-path stall); not the arena AV |

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-moeda-inteira.png`](captures/01-moeda-inteira.png) | loading | the coin without the wedge, HUD at 13.1 FPS |

## Conclusion

With copies active the app survives the menu, shows a correct coin, and then dies during the level
load — about 18 s in, which is why sampling at 30 s only ever saw a black frame. Copies off remains
the only configuration that reaches gameplay.

So Phase 2 continues **without** the copies: the magenta has to be explained on the working path.

## Next single action

Run with the new detile instrumentation (copies off, `BB_TSHARP_LOG=1`) and read `M5 detile:` to see
whether BC textures are detiled at all.
