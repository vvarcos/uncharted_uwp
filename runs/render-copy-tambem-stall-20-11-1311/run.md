# Run render-copy-tambem-stall-20-11-1311 — the copy mechanism is not the stall

Folder: `runs/render-copy-tambem-stall-20-11-1311/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 13:11 (local) |
| Commit | `6a29b73` |
| Package | `UnchartedNDCUWP_1.0.26324.1607_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0`, `BB_DEPTH_COPY_RENDER=1` |
| Watch | `watch.ps1 -Interval 5 -MaxMinutes 3` |

## Intent

Replace the failing transfers with the render-based backend (`uwp_depth_copy.cpp`, wired in
`6a29b73`) and see whether the level load survives and the colours arrive.

## Result

The backend **ran**: six copy draws were recorded.

```text
M5 render copy: 2048x2048 fmt 124 -> 124
M5 render copy: 2048x2048 fmt 124 -> 124
M5 render copy: 2048x2048 fmt 124 -> 124
M5 render copy: 1280x720 fmt 100 -> 130
M5 render copy: 1280x720 fmt 130 -> 100
M5 render copy: 1280x720 fmt 130 -> 100
```

Format 124 is `D32_SFLOAT`, 100 is `R32_SFLOAT`, so the depth and colour directions are both
covered. The app still stopped presenting about 24 s into the load, and the log repeats:

```text
GPU stall: submit #22818 scheduler … signals …=4157, waits 000…=1 000…=18661
```

## Conclusion

The copy **mechanism** is not the stall. Transfers and a full-screen render do exactly the same
thing: the copies are issued, and the GPU then waits on a semaphore that never signals — the same
submit number as the transfer path.

That moves the fault to the **use** of the copied images: once a destination image is sampled by the
following passes, something in the layout/ownership chain blocks. It is a dependency bug, not a
transfer bug.

## Next single action

Log the timeline around the stall: which semaphores belong to which queue, which submit waits on
`…=1`, and which pass first samples the copy destination. The GPU history ring already records the
last work items; the missing piece is the queue that owns the unsignalled semaphore.
