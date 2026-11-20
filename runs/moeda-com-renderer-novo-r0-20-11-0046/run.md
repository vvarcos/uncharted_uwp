# Run moeda-com-renderer-novo-r0-20-11-0046 — coin on the first real renderer build

Folder: `runs/moeda-com-renderer-novo-r0-20-11-0046/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 00:46 (local) |
| Commit | `33bae9a` |
| Package | `UnchartedNDCUWP_1.0.26324.341_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | none |
| Dump | `E:\dumps\CASA02320` / `E:\dumps\CUSA02320` |

## Intent

Read the new `R` counter on the loading coin and settle whether the missing triangle comes from
RECT_LIST.

## Why this run matters

`1.0.26324.341` is the **first** package whose layout actually contains a freshly built
`bbgpu_dll.dll`. Builds 303, 320 and 330 shipped the 23:36 renderer (the layout copy was attached to
the executable, which those builds did not relink), so their results measured old code.

## Timeline

| Time | Event |
| --- | --- |
| 00:43 | title → game selection → Friend Stats OK |
| 00:44 | main menu, `QUAD 20 idx 120 R 0` |
| 00:44 | `cross` on Continue → loading coin |
| 00:45 | coin frame captured: `QUAD 35 idx 210 R 0`, 43.9 FPS, 3 808 MiB |

## Telemetry

| Metric | Value |
| --- | --- |
| Coin screen FPS | 43.9 (22.78 ms) |
| RAM | 3 808 / 3 808 MiB |
| QUAD / indices / RECT | **35 draws, 210 indices, R 0** |
| FIN / ARENA | 2 / 2 |

## Reached gameplay?

- Verdict: **loading screen** (the coin), then the capture endpoint returned HTTP 500.

## Failures

| Kind | Detail |
| --- | --- |
| Screenshot endpoint | HTTP 500 after the load, i.e. the app stopped presenting |

## Visual symptoms

The coin keeps the V-shaped black notch with the corrected renderer on screen. Combined with
`R 0`, this **rules out** RECT_LIST, back-face culling and winding as explanations: every draw on
that screen is a QUAD_LIST expanded to six indices, both triangles are submitted, and the notch is
still there.

That leaves the shaded result itself: the transparent part of the coin's texture drawn as opaque
black (blend/alpha), or a vertex whose position/attribute is wrong for the second triangle.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-moeda-cunha-R0.png`](captures/01-moeda-cunha-R0.png) | loading | the coin, the wedge, and `R 0` in the HUD |
| [`captures/02-menu.png`](captures/02-menu.png) | menu | main menu with the new HUD |

## Conclusion

Three hypotheses are dead: rect list, culling and winding. The next measurement is the blend state
of the coin's pipeline — the PSO digest now prints `blend=<0|1>` — because a transparent region
rendered opaque explains the coin, the Bluepoint fade and the full-screen composite equally.

## Next single action

Read `blend=` from the `M5 pso:` line of the coin frame.
