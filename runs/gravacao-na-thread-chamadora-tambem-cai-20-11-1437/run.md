# Run gravacao-na-thread-chamadora-tambem-cai-20-11-1437 — not a recording-order race

Folder: `runs/gravacao-na-thread-chamadora-tambem-cai-20-11-1437/` — screenshots of this run go to
`captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 14:37 (local) |
| Commit | `f72a44d` |
| Package | `UnchartedNDCUWP_1.0.26324.1732_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0`, `BB_VK_RECORD_THREAD=0` |

## Intent

The copy is recorded through the scheduler's deferred command chunks; the barrier pair around it is
flushed separately. If the two were ordered incorrectly for a moment, the copy would run in the wrong
layout. Recording on the calling thread (`BB_VK_RECORD_THREAD=0`) removes the chunk hand-over entirely.

## Result

The app exited about 18 s after the loading input, exactly as with threaded recording.

## Conclusion

**Refuted**: the hang is not a recording-order race between the barriers and the copy.

That leaves the substitution itself as the only difference between the two worlds: with the host copy,
the GPU stops; with copies off, the game runs (in magenta). The next run drops the substitution and
lets the guest's own copy shader run (`BB_NO_COPY_HLE=1`).
