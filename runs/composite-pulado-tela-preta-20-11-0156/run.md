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

**The app survives and runs at 57 FPS, but the screen is black.** The HUD is the only thing drawn:
`57.0 FPS 17.53 ms`, `FIN 0 0ms ARENA 1`, `QUAD 33 idx 198 R 0`, RAM 3 337 / 3 570 MiB.

That is expected in hindsight: `0xb4fa3a82` is the draw that puts the scene on screen, so skipping it
cannot be a fix — it removes the picture. The old note that described "correct colours with the shader
skipped" was about a different build and a different defect (the black triangle, since fixed by the
quad triangulation).

## Conclusion

Two facts are now settled:

1. Copies **off** → the game runs and plays, with magenta textures (Phase 2.2 must explain them).
2. Copies **on** → GPU stall at the copy path, and the composite cannot be skipped to work around it.

So the copy path's stall has to be fixed on its own terms, or the magenta has to be fixed without the
copies. The next session starts by choosing between those two with evidence, not by guessing: dump
the copy submit that stalls (`BB_GPU_HISTORY=1` is already on) and check whether the composite reads
an image the copies wrote or a different one.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-tela-preta-hud.png`](captures/01-tela-preta-hud.png) | gameplay | black screen, HUD at 57 FPS |
