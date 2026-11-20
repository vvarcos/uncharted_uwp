# Run dump-do-submit-que-trava-20-11-1432 — the hung submit ends in draws, not in the copy

Folder: `runs/dump-do-submit-que-trava-20-11-1432/` — screenshots of this run go to `captures/` beside
this file; the dumped shaders are in `shader-dumps/`.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 14:32 (local) |
| Commit | `f72a44d` |
| Package | `UnchartedNDCUWP_1.0.26324.1732_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0`, `BB_GPU_HISTORY=1`, `BB_DUMP_SHADERS=0x7095d6c2,0xaec539f0,0x6a6422a9,0xc4e9ae30,0xb6059c71` |

## Intent

Read the commands of the submit that hangs instead of guessing. The crash history of the previous copy
run ended in draws and dispatches; this run dumps those shaders so they can be inspected offline.

## Result

The app exited about 18 s after the loading input, as before. The full crash history of the hanging
tick (`21812`, submit `#25797`) is now recorded, and it ends like this:

```text
GPU crash:   #326849 tick 21812 draw      f585649d 701c3d08 counts 6 1 0
GPU crash:   #326851 tick 21812 color/depth copy 00000064 00000064 counts 1280 720 0
GPU crash:   #326852 tick 21812 dispatch  6a6422a9           counts 384 1 1
GPU crash:   #326853 tick 21812 dispatch  6a6422a9           counts 3840 1 1
GPU crash:   #326854 tick 21812 color/depth copy 00000082 00000064 counts 1280 720 0
GPU crash:   #326857 tick 21812 draw      f585649d …        counts 6 1 0
GPU crash:   #326866 tick 21812 dispatch  aec539f0           counts 1 360 1
GPU crash:   #326867 tick 21812 dispatch  7095d6c2           counts 1 1 1
```

## What the dumps say

- `cs_0x7095d6c2` (the last command in the submit) has **no loop**, no atomics and no image writes: it
  is a 256-lane workgroup reduction over two storage buffers, with three correct
  `OpControlBarrier(Workgroup, Workgroup, 264)` at uniform labels, guarded by an early
  `if (ssbo_1[30] > lane)` selection. `cs_0xaec539f0` and `cs_0x6a6422a9` are the same shape, also
  without loops.
- So a runaway shader is **refuted**: no command in the submit can spin. The GPU stops on work whose
  static form is finite.
- `cs_0x6a6422a9` is the guest's own copy shader (`LocalSize 64`, one dword per lane, 256 bytes per
  workgroup); it is the same program the host normally replaces with `vkCmdCopyImage`.

## Conclusion

The hang is not the copy command, and not a shader loop. It is the state the copy leaves behind for the
draws and dispatches that follow it in the same submit — and, crucially, the copy that replaces the
guest's shader is a **substitution the port invented**. The next run tests the opposite direction:
let the guest's shader do the copy (`BB_NO_COPY_HLE=1`) and keep every host copy off.
