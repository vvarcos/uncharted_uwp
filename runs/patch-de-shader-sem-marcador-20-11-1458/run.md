# Run patch-de-shader-sem-marcador-20-11-1458 — the patch experiment is blind until it prints

Folder: `runs/patch-de-shader-sem-marcador-20-11-1458/` — screenshots go to `captures/`.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 14:52, 15:55 and 15:58 (local) |
| Commit | `eb4425f` |
| Package | `UnchartedNDCUWP_1.0.26324.1749_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_NO_COPY_HLE=1`, `BB_SHADER_PATCH=1` (the last attempt also `BB_PIPELINE_CACHE=0`) |
| Patch | `fs_0x00000000f4b18264_0.spv` with the light loop bound replaced by `min(count, 8)` |

## Sequence

1. **14:52** — the first upload was refused (`Local files must stay inside this project directory`):
   `xbox-upload.ps1` resolves local paths against `xbox/`, not the project root. The run was therefore
   an exact repeat of the guest-copy run.
2. **15:55** — patch uploaded to `LocalState/user/shader/patch/`, run still ended at the same submit
   (`aec539f0` 1×360 then `7095d6c2` 1×1).
3. **15:58** — same, with `BB_PIPELINE_CACHE=0` so the shader is retranslated instead of coming from the
   database.

## Result

All three ended in the same two dispatches at the end of a submit, with the device lost ~13 s after the
5 s stall report. The last checkpoints of the third run:

```text
GPU crash:   #406480 tick 26881 draw 00000000f585649d 00000000f4b18264 counts 6 1 0
GPU crash:   #406481 tick 26881 dispatch 00000000aec539f0 counts 1 360 1
GPU crash:   #406482 tick 26881 dispatch 000000007095d6c2 counts 1 1 1
```

**None of the logs can say whether the patch was applied**: `LOG_INFO(Loader, …)` never reaches
`loader.log` (the file has zero `<Info>` lines), and the patch branch prints nothing else. A negative
result here is unreadable, so this experiment proves nothing yet.

## Change made for the next run

`CompileShader` now prints `M5 shader patch: <name> applied|found-but-off|missing hash=… perm=…` whenever
`BB_SHADER_PATCH=1`. With that marker, the next run either confirms the patch is in use (and the light
loop can be excluded) or shows why it is not.
