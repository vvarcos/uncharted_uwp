# Run indices-pelo-buffer-cache-20-11-0055 — index reads through the buffer cache

Folder: `runs/indices-pelo-buffer-cache-20-11-0055/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 00:55 (local) |
| Commit | `33bae9a` + local change |
| Package | `UnchartedNDCUWP_1.0.26324.352_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | none |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Reach the loading coin after changing the QUAD_LIST expansion to read the guest indices through the
buffer cache (`InvalidateMemory` + `ObtainBuffer`) instead of the raw guest pointer.

## Timeline

| Time | Event |
| --- | --- |
| 00:55 | launch, title, game selection, Friend Stats OK |
| 00:55 | `cross` on **Continue** |
| 00:55 | app exits; every capture after that is the dashboard |

## Telemetry

| Metric | Value |
| --- | --- |
| Frames | none from the game after Continue |
| Log | 12.6 MB, ends in an access violation |

## Failures

| Kind | Detail |
| --- | --- |
| Access violation | `M5 av-scan: addr=0x10 access=0 rip=0x7ffd54cfe39b` |
| Preceded by | `M5 bound buffer destroyed: guest=0x1100000000 offset=6751632 size=252` |
| Cause | the known arena lifetime bug on Continue, unchanged by this build |

## Reached gameplay?

- Verdict: **no** — the run died on Continue, before any loading screen. The user then reached the
  coin through New Game on this same build and captured it: **the coin is almost entirely gone**,
  reduced to a thin slice at the top-left (`18.6 FPS`, `QUAD 35 idx 210 R 0`, 4 020 MiB).

## Captures

None in the repository: the frames here are the dashboard. The coin frame on this build is the
user's capture, described above.

## Conclusion

The cache read **was** evaluated after all, on the user's own run, and it made the defect worse: with
the raw pointer the coin was mostly visible with a V notch; reading through `BufferCache` left only a
thin slice. So the cache mapping is not the copy this draw needs; the change was reverted in
`vk_rasterizer.cpp` with that reasoning recorded in the code.

The coin responds to the index path, which means the indices reach the draw, but neither the raw
guest memory nor the cache mapping produces the guest's intended four corners for this draw. The
next measurement is the vertex side (`BB_FORCE_VB_UPLOAD=1`).

## Next single action

Run the New Game path with `BB_FORCE_VB_UPLOAD=1` and compare the coin against
`runs/moeda-com-renderer-novo-r0-20-11-0046/captures/01-moeda-cunha-R0.png`.
