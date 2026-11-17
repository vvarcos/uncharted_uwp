# Continue: packet-scoped arena lifetime experiment

Status: **experimental and opt-in**, not a rendering fix or M5 completion.

## Failure isolated on console

A diagnostic at the end of resource binding found two buffers already destroyed before the same command's descriptor update. Both referenced an arena based at guest address `0x1100000000`; the bindings were 32 and 252 bytes. An access violation reading `0x10` followed immediately in the Dozen descriptor-update path.

A later binding can replace a demand-sized arena while descriptors captured earlier in the same packet still hold its old Vulkan handle. Calling `scheduler.Finish()` during replacement only drains commands already recorded; it does not protect handles in the packet still being assembled.

## Experimental correction

Set the following on an extra line of the console's private `run.txt`:

```text
BB_ARENA_PACKET_RETIRE=1
```

With this flag, single-page replacement unpublishes the old arena but defers destroying it. The next draw/dispatch packet boundary calls `Finish()` and releases the replaced windows from the preceding packet. A window revisited within the same packet can be reused instead of allocated again. This is not the unbounded `BB_ARENA_FREE=0` experiment.

The code covers direct/indirect draw and compute-record paths. Cross-page merge retirement is not covered; neither its safety nor long-run memory/performance is established by this test. The default remains unchanged pending broader validation.

## Actual console A/B, same build

- Package `1.0.26321.1415`, manual Continue, copy skipping still enabled: the user reached the boat scene. A fresh capture showed Nathan and Elena in gameplay, but severe magenta/cyan materials, black geometry and about 3 FPS. Current app RAM in that capture was **4411 / 5120 MiB**; this is not a certified lifetime peak.
- The collected log contained `M5 arena packet retire: count=1 released=65536 KiB` and no new destroyed-buffer, access-violation, allocation-failure or device-removal markers.
- Changing only `BB_SKIP_COPY=0` on the same build reproduced a GPU stall. This time the real 1280x720 depth/color conversions executed (copies #4 through #10, five 4500 KiB scratch requests), unlike prior title-menu-only tests. The captured log ended in GPU-stall submission diagnostics, not a newly demonstrated null-buffer AV.
- Copy skipping was restored after collection. Game images, raw logs, console settings and binary/PDB snapshots remain private and are not committed.

## Remaining work

The observed lifetime failure no longer reproduced in the short copy-skipping test. This does **not** prove sustained gameplay, correct arena data coherence, valid null descriptors, correct topology or correct rendering. The loading-coin wedge remains unresolved. The transfer-path stall with copies enabled is now a separate reproducible target.

A later unpublished render-based copy experiment (`.1515`, ordinary descriptor sets, no full-frame scratch) also reproduced device loss. Seven copy draws were recorded; timeline batches through 18123 completed, while batch 18124 stalled. That batch included many scene draws as well as copies, so this does not isolate the fault to one copy instruction or prove a driver transfer defect. A per-copy completion-boundary diagnostic is the next step; the render backend is not a validated replacement.
