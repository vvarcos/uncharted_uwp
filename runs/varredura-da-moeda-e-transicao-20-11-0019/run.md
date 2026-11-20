# Run varredura-da-moeda-e-transicao-20-11-0019 — the triangle is one giant triangle

Folder: `runs/varredura-da-moeda-e-transicao-20-11-0019/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 00:19 (local) |
| Commit | `e56bf6d` (winding fix committed) |
| Package | `UnchartedNDCUWP_1.0.26324.303_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_ARENA_PACKET_RETIRE=1` |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Capture the loading coin with the winding fix installed, by scanning frames back to back right after
the offline Friend Stats confirmation.

## Timeline

| Time | Event |
| --- | --- |
| 00:17 | launch, title, game selection, Friend Stats OK |
| 00:17–00:18 | 14 screenshots back to back (about 1.5 s each) |
| 00:18 | frames `s-09`–`s-11` show the transition: a **brown triangle** over a dark image |
| 00:18 | frames `s-12` and later show the jungle cutscene |

## Telemetry

| Metric | Value |
| --- | --- |
| Transition FPS | 14.2 (70.54 ms) |
| RAM | 4 178 / 4 178 MiB |
| FIN / ARENA / QUAD in the transition | 2 / 2 / 21 draws, 126 indices |

## Reached gameplay?

- Verdict: **cutscene** in this run. Gameplay evidence came from the user's own captures on the
  same build: **4.1–4.9 FPS**, FIN 21–68 calls at 243–1656 ms, ARENA 21–68, RAM ~4.5 GiB, the whole
  scene magenta/cyan/purple with black blocks, and the same giant triangle.

## What the captures prove

The defect is **one giant triangle**, not a notch cut out of a quad:

- on the loading screen it looks like a V-shaped wedge because only that triangle covers the coin;
- at the transition it is a full brown triangle over the dark image, with a diagonal seam;
- in gameplay it is the "tent" across the screen, brown on top, black below.

This rules out a simple half-quad culling explanation: a culled triangle would leave the other half
of the same quad, never a free-floating triangle across the screen.

## Failures

| Kind | Detail |
| --- | --- |
| Device loss | none |
| Assert | none |
| Process exit | none in this run |

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-transicao-triangulo-marrom.png`](captures/01-transicao-triangulo-marrom.png) | transition | brown triangle over a dark image |
| [`captures/02-transicao-2.png`](captures/02-transicao-2.png) | transition | same frame family |
| [`captures/03-cutscene-selva.png`](captures/03-cutscene-selva.png) | cutscene | jungle, 53.6 FPS |

The user's gameplay captures (magenta scene, giant triangle, ammo HUD) are described above but are
not stored in the repository, per the no-game-imagery rule.

## Conclusion

The winding change did not remove the artifact, and the evidence now says the artifact is a single
triangle with the wrong transform, vertices or material — not a culling issue at the quad level.
Two candidate mechanisms remain: the composite draw reading uninitialised colour/depth because
`BB_SKIP_COPY=1` is the default in this mode, or a vertex/attribute fault in that same draw.

## Next single action

Run gameplay with `BB_SKIP_COPY=0` (copies active). If the magenta and the triangle disappear, the
defect is the skipped copy; if they remain, instrument the composite draw's vertices.
