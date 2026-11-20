# Run unidades-bc-refutadas-20-11-1429 — Dozen wants pixels, not blocks

Folder: `runs/unidades-bc-refutadas-20-11-1429/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 14:29 (local) |
| Commit | working tree on top of `4ad00ba` (the change was reverted) |
| Package | `UnchartedNDCUWP_1.0.26324.1723_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | none (copies off, the stable configuration) |

## Intent

The spec says `bufferRowLength`/`bufferImageHeight` are in texel *blocks* for compressed formats,
while this port handed the driver pixels. The earlier one-sided division had crashed, but a subagent
showed the crash could be the size helper dividing a second time. This run converted **both** sides:
the three copy sites emit blocks, and `BufferImageCopySize` now takes the values as the driver does.

## Result

The app died during the level load, before presenting a frame:

```text
Buffer cache: demand arena 64 MiB at 0x101c000000 for request 2.8 MiB
STOP: GPU library assertion failed (see GPU log above)
```

`errors.log` names the failing check:

```text
GPU [Debug] <Critical> vk_platform.h:80 Check: Assertion Failed!
vk::Result=ErrorOutOfHostMemory
```

The assertion comes out of the Dozen ICD (`vulkan_dzn` in the stack trace), at the first BC upload.

## Conclusion

**Refuted, with a hard signature.** Dozen does not accept block units here: given 512 instead of
2048 it fails with `VK_ERROR_OUT_OF_HOST_MEMORY`, which is not a plausible result of a smaller row
length — it is Dozen taking a different (allocating) path for what it reads as a short row. Pixels are
what this ICD wants, and with pixels the app is stable.

That also closes the "the BC upload is what makes the materials magenta" lead: the pitch unit is not
the defect. The whole change (both copy sites and `BufferImageCopySize`) was reverted; the tree is
back to the known-good state.

## Next single action

Stop looking at the upload and look at the draw that actually hangs: the crash history of the last
copy run ends in draws and guest compute dispatches, not in the copy (`runs/qualquer-copia-derruba-20-11-1343/`).
Dump those shaders (`BB_DUMP_SHADERS=0x…`) and read them.
