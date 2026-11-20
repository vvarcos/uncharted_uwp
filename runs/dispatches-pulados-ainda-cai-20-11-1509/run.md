# Run dispatches-pulados-ainda-cai-20-11-1509 — the crash is not tied to the programs

Folder: `runs/dispatches-pulados-ainda-cai-20-11-1509/` — screenshots go to `captures/`.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 15:09 (local) |
| Commit | `b7d12c0` |
| Package | `UnchartedNDCUWP_1.0.26324.1759_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_NO_COPY_HLE=1`, `BB_SKIP_SHADERS=aec539f0,7095d6c2` |

## Intent

Localise the hang inside the submit: skip exactly the two compute dispatches that close it and see
whether the load passes.

## Result

The skip took effect — `aec539f0` is gone — and the submit now ends with a **different** program:

```text
GPU crash:   #435116 tick 29499 draw f585649d f4b18264 counts 6 1 0
GPU crash:   #435117 tick 29499 dispatch 04f81a8f counts 1 1 1
```

The app still died ~18 s after the loading input, with the same 5 s stall report.

## Conclusion

**The hang is not tied to a program.** Replace the tail dispatches and another one takes their place at
the same point of the frame; the device is still lost.

Looking across all copy runs, the crash tick moves a lot (21 812, 21 967, 22 977, 26 881, 28 408,
29 499): it is not a fixed frame, it is a condition reached after the level load has been running for a
while. Together with the 5 s stall and the ~13 s wait before `DEVICE_LOST`, the only class left that
fits is the **resource state**: with copies skipped the function returns before any layout transition,
so the copy-on runs are also the only ones that ever transition these images to transfer layouts.

The next run separates that from the content: `BB_COPY_BARRIERS_ONLY=1` executes exactly the same
barrier pair and records no copy at all.
