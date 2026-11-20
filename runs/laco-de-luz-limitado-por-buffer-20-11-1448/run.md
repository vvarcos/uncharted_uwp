# Run laco-de-luz-limitado-por-buffer-20-11-1448 — the last draw before the hang is a light loop

Folder: `runs/laco-de-luz-limitado-por-buffer-20-11-1448/` — the twelve disassembled pixel shaders of
the hanging submit are next to this file; screenshots go to `captures/`.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 14:48 (local) |
| Commit | `a5a9420` |
| Package | `UnchartedNDCUWP_1.0.26324.1738_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_NO_COPY_HLE=1`, `BB_GPU_HISTORY=1`, `BB_DUMP_SHADERS=0x701c3d08,…,0xa15b09b` |

## Intent

With the copy ruled out, dump every pixel shader of the submit that hangs and look for a loop whose
bound comes from data. A data-dependent loop is the one shape that turns "the copy filled real data"
into a multi-second GPU submit (the port waits 5 s, reports a stall, and the device is lost ~13 s
later — that is a timeout, not a deadlock).

## Result

The app died at the same point, as before. Of the twelve shaders, three have loops; two are bounded by
constants. The third, **`fs_0x00000000f4b18264`** — the last 6-vertex draw before the two dispatches —
has this loop:

```text
LoopMerge %77 %76
%74: %259 = OpULessThan %bool_id %256 %247     ; %256 = 0,1,2,…   %247 loaded from ssbo_1
     … loads four floats per iteration from ssbo_1 and does a light-attenuation term
```

`%247` is `OpLoad` from `ssbo_1[buf0_dword_off + 72]`: the **light count comes from guest memory**, not
from a constant. Reading a float's bit pattern there (e.g. `0x3F800000`) as an unsigned count means
~1 000 million iterations per pixel, which is exactly a multi-second submit.

## Why this fits every earlier measurement

- With host copies, with the render backend, and with the guest's own copy shader the same submit hangs:
  the loop is in a draw that follows the copied data, not in the copy.
- With the copy skipped the destination images stay empty, the count stays small/zero, and the game runs
  (in magenta).
- The stall report is a 5 s timeout with a healthy counter, and the device is only lost ~13 s later — the
  signature of a very long submission, not of a deadlock.

## Next single action

Prove it: patch that one program so the loop bound is `min(count, 8)`, enable shader patches
(`BB_SHADER_PATCH=1`, the path was disabled on this port) and run with `BB_NO_COPY_HLE=1`. If the level
load passes with the clamp, the count is the poison and the real fix is on the producer side.
