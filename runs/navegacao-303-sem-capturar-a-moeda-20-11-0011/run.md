# Run navegacao-303-sem-capturar-a-moeda-20-11-0011 — first session on the winding fix

Folder: `runs/navegacao-303-sem-capturar-a-moeda-20-11-0011/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 00:11 (local) |
| Commit | `190a636` + uncommitted fixes, **with** the QUAD_LIST winding correction |
| Package | `UnchartedNDCUWP_1.0.26324.303_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_ARENA_PACKET_RETIRE=1` |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Reach the loading coin on the fixed binary and compare it with
`runs/moeda-sem-o-fix-instalado-20-11-0004/captures/01-moeda-com-cunha.png`.

## Setup

- Version confirmed as `1.0.26324.303` in the install output — the first run where the installed
  build really contains the winding change.
- Input: scripted `cross` / `down` through `LocalState/pad.txt`.

## Timeline

| Time | Event |
| --- | --- |
| 00:07 | launch, title, main menu (48.8 → 43.5 FPS) |
| 00:08 | `down` ×3: Continue → New Game, confirmed by capture |
| 00:09 | `cross`: offline Friend Stats, then **Game Mode** (Campaign / Continuous Speed Run) |
| 00:09 | `cross` ×3: Campaign → difficulty → the intro cutscene starts |
| 00:10 | jungle cutscene, 57.1 FPS |

## Telemetry

| Metric | Value |
| --- | --- |
| Main menu FPS | 43.5–48.8 |
| Cutscene FPS | 57.1 (17.51 ms) |
| RAM peak in this session | 4 421 MiB (cutscene) |
| FIN / ARENA / QUAD on the menu | 6–8 / 6–8 / 36–46 draws |
| FIN / ARENA / QUAD in the cutscene | 0 / 0 / 9 draws, 54 indices |

## Reached gameplay?

- Verdict: **cutscene**

## Failures

| Kind | Detail |
| --- | --- |
| Device loss | none |
| Assert | none |
| Process exit | none |

## Visual symptoms

The menu and the cutscene render without the wedge. **The loading coin itself was never captured**:
the sequence skipped from difficulty straight into the cutscene, so there is no frame of the coin in
this run. The fix is therefore not yet verified on the coin, the Bluepoint logo or the gameplay
composite.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-menu-continue-ambushed.png`](captures/01-menu-continue-ambushed.png) | menu | main menu, Continue (Ambushed) |
| [`captures/02-game-mode-campaign.png`](captures/02-game-mode-campaign.png) | menu | Game Mode screen |
| [`captures/03-cutscene-selva.png`](captures/03-cutscene-selva.png) | cutscene | intro jungle, 57.1 FPS |

## Conclusion

The fixed build runs the menus and the intro without regressions, but this run does not answer the
question it was opened for. Timing, not code, is the obstacle: the loading screen is short and the
scripted presses run several seconds apart because each press is an HTTP upload.

## Next single action

Capture the load window with a tight loop (one screenshot every ~1.2 s starting immediately after
the difficulty confirm), as `runs/` entry for the next session does.
