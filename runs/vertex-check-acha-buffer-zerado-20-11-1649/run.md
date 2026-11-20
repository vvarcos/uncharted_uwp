# Run vertex-check-acha-buffer-zerado-20-11-1649 — a vertex buffer that is mostly zeros, with two bad elements

Folder: `runs/vertex-check-acha-buffer-zerado-20-11-1649/` — capture in `captures/01-82s.png`.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 16:49 (local) |
| Commit | `1257f15` (the package predates the new `M5 vertex suspect` log) |
| Package | `UnchartedNDCUWP_1.0.26324.1809_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_VERTEX_CHECK=1` |

## Result

The existing check fired on the first frames of the level:

```text
Vertex check: frame 0 vs fadcd6fe buffer 2 at 0x1005e0f2b0 stride 64, 4096 elements: 2 insane (first 30), 3992 zero
Vertex check: frame 0 vs fadcd6fe buffer 0 at 0x1005e0f2a0 stride 64, 4096 elements: 0 insane (first 0), 3988 zero
Vertex check: frame 0 vs fadcd6fe buffer 1 at 0x1005e0f2c0 stride 64, 4096 elements: 0 insane (first 0), 4004 zero
```

Three streams of the same vertex shader (`fadcd6fe`), stride 64, 4 096 elements each, and almost every
element is **zero**; buffer 2 additionally has two elements (`30` and `31`) that fail the sanity test.

A stream of zeros produces degenerate triangles; two insane elements next to a field of zeros are exactly
the shape of "a vertex now and then stretches geometry across the screen".

## What this does and does not say

- It says the defect is a **vertex stream that reaches the shader half empty**, not a bad bone matrix
  (the bone checks stayed quiet) and not a texture.
- It does not say yet *why* the stream is empty: the check only samples guest memory at frame 0; the
  upload may happen later, or the range may be GPU-written and never synchronized. The new
  `M5 vertex suspect: addr=… stride=… offset=… draw_hash=… reason=…` log (commit `60c9be9`) reports the
  same case at stream-assembly time, with the address and the draw hash, which is what a fix needs.
- The run ended after 1.5 minutes without a GPU crash report in the log (the app left no `GPU crash:`
  line), so this exit is not the copy timeout.

## Next

Build `60c9be9` together with the prompt fix, then run `BB_VERTEX_CHECK=1` for a longer session and read
`M5 vertex suspect` lines: the first one that repeats at the moment of a visible explosion names the
buffer and the draw.
