# Run sem-espera-no-submit-culpado-20-11-1408 — the failing submit waits for nothing

Folder: `runs/sem-espera-no-submit-culpado-20-11-1408/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 14:08 (local) |
| Commit | `8c18949` |
| Package | `UnchartedNDCUWP_1.0.26324.1705_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0` |

## Intent

Match the semaphore that never signals against the bridge's acquire/ready pair, using the new
handles line.

## Result

```text
M5 bridge semaphores: acquired=0000024B900F5B60 ready=0000024B900F43C0
GPU stall: submit #21695 scheduler … signals …=17148, waits
GPU stall: submit #21696 scheduler … signals …=17149, waits
GPU stall: scheduler … timeline …: counter_result=-4 reached=0 last_known=17173 next_tick=17175
```

The submits at the failure have an **empty wait list**, and none of the bridge handles appear. The
earlier `…=1` wait belonged to a different run with different handles.

`counter_result=-4` is `VK_ERROR_DEVICE_LOST`; `reached=0` while `last_known=17173` means the device
is gone and the counter cannot even be read.

## Conclusion

The semaphore-leak hypothesis is **refuted**: the failing submit waits for nothing, so no dependency
can be blamed. The GPU work itself faults when a copy is submitted.

What is left, and it is now narrow: the resources the copy touches. A missing usage flag or a layout
the D3D12 backend mishandles causes exactly this — an unrecoverable device loss with no fault report,
because Dozen does not expose `VK_EXT_device_fault`.

## Next single action

Log the source and destination `vk::ImageUsageFlags` at the copy site. A missing
`TRANSFER_SRC`/`TRANSFER_DST` (transfers) or `COLOR_ATTACHMENT`/`DEPTH_STENCIL_ATTACHMENT` (render
backend) would explain a fault in both mechanisms at once.
