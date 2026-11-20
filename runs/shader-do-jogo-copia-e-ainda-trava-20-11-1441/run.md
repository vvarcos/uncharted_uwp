# Run shader-do-jogo-copia-e-ainda-trava-20-11-1441 — the copy is not the trigger, the copied data is

Folder: `runs/shader-do-jogo-copia-e-ainda-trava-20-11-1441/` — screenshots of this run go to
`captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 14:41 (local) |
| Commit | `d596678` |
| Package | `UnchartedNDCUWP_1.0.26324.1738_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_NO_COPY_HLE=1` (guest copy shader runs; no host copy at all) |

## Intent

Every host copy path (transfer, staging, render) hangs the GPU, so stop substituting and let the guest's
own copy shader do the work. `IsComputeImageCopy` normally replaces that shader with `vkCmdCopyImage`;
this run disables the replacement.

## Result

**No host copy ran at all** — the log has no `M5 copy` line — and the app still stopped at the same
point in the level load:

```text
GPU crash:   #330852 tick 21967 dispatch aec539f0 counts 1 360 1
GPU crash:   #330853 tick 21967 dispatch 7095d6c2 counts 1 1 1
GPU stall: scheduler … timeline …: counter_result=-4 reached=0 last_known=21966 next_tick=21968
```

The same two dispatches that end the hung submit in the host-copy runs also end it here, at the same
place in the load.

## Conclusion

The copy **command** is now completely ruled out: three host mechanisms and the guest's own shader all
reach the same submit and the same two dispatches, and the app dies there with the device lost.

What all four configurations share is that the destination images hold **real data**. With the
replacement on and `BB_SKIP_COPY=1`, the copy is dropped, the images stay empty, and the game plays
(magenta). So the hang follows the data, not the copy.

A shader cannot spin here — the three dumped programs of that submit have no loop (`runs/dump-do-submit-que-trava-20-11-1432/shader-dumps/`).
The next run therefore separates the two remaining classes: drop every compute dispatch
(`BB_SKIP_COMPUTE=1`) and see whether the hang is in a dispatch or in the draws that follow.
