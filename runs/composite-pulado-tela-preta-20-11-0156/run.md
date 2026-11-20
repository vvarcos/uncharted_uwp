# Run composite-pulado-tela-preta-20-11-0156 — skipping the composite is a black screen

Folder: `runs/composite-pulado-tela-preta-20-11-0156/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 01:56 (local) |
| Commit | `00db9a4` |
| Package | `UnchartedNDCUWP_1.0.26324.436_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0`, `BB_SKIP_SHADERS=0xb4fa3a82` |
| Dump | `E:\dumps\CUSA02320` |

## Intent

The A/B the original notes specified: keep the colour/depth copies active (the GPU stall) and skip
the composite fragment shader `0xb4fa3a82`, to see whether the stall belongs to that draw and whether
the colours are correct without it.

## Result

The capture was taken **too early**: the user watched the console and the load had already moved past
this black interval. A black frame at 57 FPS with a live HUD is the loading gap between the movie and
the scene, not the effect of the skipped shader.

The app was alive and running; the run does not prove that skipping the composite blanks the screen,
and it does not prove the stall either. It is a void measurement and must be repeated with a long
wait (90–120 s) before any conclusion.

That is expected in hindsight: `0xb4fa3a82` is the draw that puts the scene on screen, so skipping it
cannot be a fix — it removes the picture. The old note that described "correct colours with the shader
skipped" was about a different build and a different defect (the black triangle, since fixed by the
quad triangulation).

## Conclusion

This run is **void**: the frame is the loading gap, captured before the scene appeared. Nothing here
says whether the composite can be skipped or whether the copy stall resolves given time.

What stands from the previous run: with copies on, the app stopped presenting and the log repeated
`GPU stall: submit #…` for 25 632 submits. Whether that is a permanent stall or a very slow path has
to be measured with a long wait.

## Next single action

Repeat copies-on **without** skipping any shader, wait 120 s, and capture; if the scene appears, the
"stall" was duration, not a hang.
