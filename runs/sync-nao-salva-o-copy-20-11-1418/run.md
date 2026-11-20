# Run sync-nao-salva-o-copy-20-11-1418 — waiting after the copy does not save it

Folder: `runs/sync-nao-salva-o-copy-20-11-1418/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 14:18 (local) |
| Commit | `4508e12` |
| Package | `UnchartedNDCUWP_1.0.26324.1715_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0`, `BB_COPY_SYNC=1` |
| Watch | `watch.ps1 -Interval 5 -MaxMinutes 3` |

## Intent

The copy is recorded into a deferred command with raw `vk::Image` handles; the texture cache could
evict one before the submit. Waiting for the GPU immediately after each copy tests that lifetime
hypothesis directly.

## Result

The app exited about 18 s into the load, exactly like every other copy configuration.

## Conclusion

**Image lifetime is refuted**: even when the copy completes before anything else runs, the device is
lost. So the fault is the command itself — the copy operation or the barrier pair around it — not a
reference to a destroyed image.

The remaining explanations, in order:

1. the layout transition pair (`eShaderReadOnlyOptimal`/`eColorAttachmentOptimal` or
   `eDepthStencilAttachmentOptimal`) that the copy forces, in a combination Dozen mishandles;
2. the copy's extent/format against the image's real tiling, which Dozen accepts and then faults on;
3. the scratch/staging allocation used by the transfer, though the render backend without scratch
   faults identically.

## Next single action

Cut the copy down to the smallest case: a 1280x720 R32 image copied to a freshly created image with
the same usage flags, no guest images involved, and check whether the device survives. That separates
"the copy operation is broken" from "these specific guest images are".
