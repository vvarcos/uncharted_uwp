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

A later unpublished render-based copy experiment (`.1515`, ordinary descriptor sets, no full-frame scratch) also reproduced device loss. Seven copy draws were recorded; timeline batches through 18123 completed, while batch 18124 stalled. That batch included many scene draws as well as copies, so this does not isolate the fault to one copy instruction or prove a driver transfer defect. The render backend is not a validated replacement.

Follow-up `.1522` completion boundaries showed all seven isolated render copies finishing (last tick 12843), with device loss in the following batch 12844. Automatic `.1558` navigation confirmed saved-game Continue and the offline notice without user input, then reproduced the same pattern: all seven copies completed through tick 57487, followed by a stall at 57488. This was not a verified gameplay frame. A separate single-compute skip (`aec539f0`) still lost the device after its seven completed copies, so that shader is not established as the sole cause. The next target is the individual draw/dispatch packets following the final completed copy; neither a copy checkpoint nor the last shader hash identifies the fault by itself.

## Fault isolated to one draw (.1613)

Fencing every command only after the seventh copy completed showed the earlier work finishing: two `6a6422a9` dispatches and one `f585649d`/`7d6cd44f` draw completed, then a draw with vertex shader `f585649d` and fragment shader `b4fa3a82` stalled and the device was lost. The copies were not what stalled.

With that one fragment shader skipped, and the colour/depth copies still **active**:

- seven copy fences per frame completed with no stall;
- the scene rendered with correct colours for 5838 frames at roughly 35-57 FPS;
- sampled RAM stayed near 3711-3755 MiB of the 5120 MiB ceiling;
- the log contained no `device was lost`, no GPU stall and no new access violation.

That is not a fix. Skipping a fragment shader is a diagnostic workaround, and it leaves a large black triangular region — the same triangular defect seen on the loading coin, which strongly suggests both come from this draw rather than from the copy path.

## The exit is GPU loss, not a clean close

Adding shutdown markers (CoreWindow Closed handler and the event-loop return) never fired on exit. An instrumented rerun with the composite active logged `GPU stall: waiting 5 s` and then `GPU crash: the graphics device was lost`. The app disappearing is therefore a consequence of the lost device, not a separate window-close path, and RAM was around 3.4-3.7 GiB of the 5120 MiB ceiling, so neither memory pressure nor a graceful close explains it.

A guard that stops non-multiple-of-four index counts from taking the QUAD_LIST expansion path did not prevent the loss either: the stall simply moved to the same composite draw. The composite fragment shader itself is now the primary suspect rather than the geometry feeding it.

The 600-second gameplay gate is **not** met.

### Next diagnostic, already specified

The composite's SPIR-V has never been dumped: the existing shader dumps cover only compute programs, and that was a limitation of the dump filter rather than of the tooling (the dumper is stage agnostic). Running with `BB_PIPELINE_CACHE=0` and `BB_DUMP_SHADERS=0xb4fa3a82` should emit that fragment shader for offline disassembly, which is the next step before any claim about why the device is lost. The draw is identified by shape as the scene composite (`OnSceneComposite`), not by hash, and it is not the jitter pass.
