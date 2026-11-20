# Run barreiras-sem-copia-jogo-roda-20-11-1515 — the barrier pair is innocent, the content is the trigger

Folder: `runs/barreiras-sem-copia-jogo-roda-20-11-1515/` — the two captures are `captures/01-gameplay-127s.png`
and `captures/02-gameplay-110s.png`.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 15:15 (local) |
| Commit | `bc75dfd` |
| Package | `UnchartedNDCUWP_1.0.26324.1809_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0`, `BB_COPY_BARRIERS_ONLY=1` |

## Intent

With copies skipped, `CopyColorAndDepth` returns **before** any layout transition, so no run has ever
separated the transfer barrier pair from the copied content. `BB_COPY_BARRIERS_ONLY=1` does exactly the
same `Transit` pair (`src → eTransferSrcOptimal`, `dst → eTransferDstOptimal`, flush) and records **no
copy**: identical resource state, zero bytes written.

## Result

The app **survived 2.2 minutes** and reached the boat scene:

- 24 frames captured, the last ones 2.4 MB each (a full 3D image, not a black screen);
- HUD, the pistol and the tutorial prompt are on screen;
- the debug overlay reads `3.0 FPS 314.41 ms`, `RAM 4457/4590/5120 MiB`;
- the scene colours are still the magenta/green corruption.

## Conclusion

| Component | Verdict |
| --- | --- |
| The transfer barrier pair | **innocent** — same barriers, game runs |
| Layout state left behind | **innocent** for the crash |
| The copied content | **the trigger** — the only thing that differs |
| The copies as a colour fix | **not sufficient**: the scene is equally magenta with them disabled |

So the crash is not a layout or barrier problem at all: it needs the data the copy puts in the
destination. And since the scene renders the same way with the copy disabled, the magenta has a cause of
its own — it does not wait for the copy path.

The 3 FPS figure is the frame rate of this configuration (the copies are disabled but the barriers
remain), so it says nothing new about performance.

## Next single action

Profile the GPU with the copies really on (`BB_GPU_PROFILE=1`): the last per-pass timings before the
stall name the pass that explodes when it sees the copied data.
