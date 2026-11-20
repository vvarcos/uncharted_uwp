# Run patch-aplicado-ainda-trava-20-11-1503 — the light loop was patched and the load still dies

Folder: `runs/patch-aplicado-ainda-trava-20-11-1503/` — screenshots go to `captures/`.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 15:03 (local) |
| Commit | `78b2379` |
| Package | `UnchartedNDCUWP_1.0.26324.1759_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_NO_COPY_HLE=1`, `BB_SHADER_PATCH=1`, `BB_PIPELINE_CACHE=0`, `BB_TRACE_BUFFERS=f4b18264` |

## Intent

Make the patch experiment readable and test the data-dependent light loop at the same time: the
program is replaced by one whose loop bound is `min(count, 8)`, and the new marker says whether the
shader was really replaced.

## Result

The marker is unambiguous:

```text
M5 shader patch: fs_0x00000000f4b18264_0 applied hash=f4b18264 perm=0
```

The program has a single permutation, so the patched module is what the draw uses. The app still died
at the same point, in the same submit:

```text
GPU crash:   #430431 tick 28408 draw f585649d f4b18264 counts 6 1 0
GPU crash:   #430432 tick 28408 dispatch aec539f0 counts 1 360 1
GPU crash:   #430433 tick 28408 dispatch 7095d6c2 counts 1 1 1
```

The trace also shows that the buffer the shader's *first* binding points at is a 29-record float32
uniform block (`stride 16, records 29` = 464 bytes, camera vectors), and its `+288` window is zero. The
light list is not that binding, so this trace cannot show the light count — but with the loop clamped
the question is moot.

## Conclusion

**The data-dependent light loop is refuted as the cause of the timeout.** It was replaced in the
module that actually ran, and nothing changed.

What remains at the end of the submit is the pair of compute dispatches (`aec539f0` 1×360,
`7095d6c2` 1×1) and the draws just before them. The next run skips exactly those two dispatches
(`BB_SKIP_SHADERS`) and says whether the hang is in them or in the draws.
