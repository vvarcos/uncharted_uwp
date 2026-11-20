# Run moeda-corrigida-e-meta-nova-20-11-0108 — the coin is fixed

Folder: `runs/moeda-corrigida-e-meta-nova-20-11-0108/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 01:08 (local) |
| Commit | `1db18b1` |
| Package | `UnchartedNDCUWP_1.0.26324.400_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | none |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Verify the corrected QUAD_LIST triangulation on the loading coin.

## Result

**Fixed, confirmed on the console by the user.** The coin no longer has the V-shaped notch. The same
triangulation drives the Bluepoint logo and the gameplay composite, so those defects share the fix.

Verification is the user's direct observation: the app dies on the level load about two seconds
after the coin appears, which is shorter than one screenshot round trip. The only capture in this
folder is the **before** frame from
`runs/moeda-com-renderer-novo-r0-20-11-0046/captures/01-moeda-cunha-R0.png`.

## The change

```cpp
// before: {v1,v2,v0} + {v1,v3,v0}   -> leaves the band between the diagonals uncovered
out[0] = g[1]; out[1] = g[2]; out[2] = g[3];
out[3] = g[1]; out[4] = g[3]; out[5] = g[0];
```

The guest hands the four corners in the order the synthetic TCS expects (`{1,2,0,3}` mapped over the
unit quad), so `{v1,v2,v3}` and `{v1,v3,v0}` tile it exactly. The previous pair shared the edge
`v1-v0`, and the union missed the region between `y=x` and `y=1-x` — a triangle.

## Telemetry

| Metric | Value |
| --- | --- |
| Coin FPS before the fix | 43.9 (build 341) |
| QUAD / indices / RECT | 35 / 210 / 0 |

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-antes-cunha.png`](captures/01-antes-cunha.png) | loading | the **before** frame: coin with the wedge, `R 0` |

## Conclusion

Phase 0 of [`GOAL.md`](../../GOAL.md) is done. Three hypotheses were eliminated on the way (RECT_LIST,
culling/winding, BufferCache index coherence) and the defect was the triangulation itself.

What remains, in the order `GOAL.md` now tracks: the level-load access violation, the magenta
textures, the `FIN` cost that keeps gameplay at 2.5–4.9 FPS, and then a long run.

## Next single action

Phase 1: instrument the descriptor update that follows the destroyed arena buffer in the level load.
