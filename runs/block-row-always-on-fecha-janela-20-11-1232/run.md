# Run block-row-always-on-fecha-janela-20-11-1232 — the block row length broke startup

Folder: `runs/block-row-always-on-fecha-janela-20-11-1232/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 12:32 (local) |
| Commit | `f6d5949` |
| Package | `UnchartedNDCUWP_1.0.26324.1519_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | none (the change was unconditional) |

## Intent

Phase 2.2: upload block-compressed images with `bufferRowLength`/`bufferImageHeight` in **blocks**, as
Vulkan requires, to remove the magenta from every BC material.

## What happened

- First launch reached the game: the raw log ends with `submit=750/749 flip=749/749` and 8 312 quad
  draws, then `M5 ASSERT vk_platform.h:80` during a burst of 256 KiB–2.75 MiB texture-image
  allocations.
- Every launch after that came back to the dashboard with the process lingering; the window never
  presented again.

## Failures

| Kind | Detail |
| --- | --- |
| Assert | `M5 ASSERT vk_platform.h:80 kind=assert-msg` (a `VkResult` check) after hundreds of texture allocations |
| Startup | the window stopped appearing entirely on subsequent launches |
| Device loss | not printed |

## Reached gameplay?

- Verdict: **partially** — 749 presented frames in the first launch, no capture of that window.

## Conclusion

The change is not safe as written: it either makes the upload fail (the `VkResult` assert) or leaves
the presenter unable to start. It is now **opt-in** behind `BB_BLOCK_ROW_BLOCKS=1`, the previous
behaviour is the default, and `1.0.26324.1530` boots normally again.

The underlying observation still stands — Vulkan counts row length in blocks for compressed formats —
but the upload path cannot simply divide the pitch: the extent and the mip offsets are in pixels, and
something in the pair is inconsistent.

## Next single action

With `1.0.26324.1530`, enable only `BB_BLOCK_ROW_BLOCKS=1` and check whether it boots; if it does,
compare the colours before deciding whether the division belongs in the upload or in the mip layout.
