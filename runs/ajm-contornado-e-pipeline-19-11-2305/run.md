# Run ajm-contornado-e-pipeline-19-11-2305 — past the audio import, rejected pipeline

| Field | Value |
| --- | --- |
| Date | 2026-11-19 23:05 (local) |
| Commit | `190a636` + uncommitted HUD/telemetry changes |
| Package | `UnchartedNDCUWP_1.0.26324.128_x64__70t7zv47487b6` (M5 diagnostic profile) |
| Build stamp | `Build Nov 19 2026 22:28:18` |
| Run mode (`run.txt` line 1) | `entry-realgnm-draw` (rewritten to `entry`) |
| Switches | `BB_SKIP_COPY=1`, `BB_UNRESOLVED=log-fail` |
| Dump / patch | `E:\dumps\CUSA02320` |

## Intent

Get past the `sceAjmInitialize` stop using the documented `BB_UNRESOLVED=log-fail` switch, and see
which subsystem fails next.

## Setup

- `LocalState/Prepared`: `boot-linked.bin` + `content.bin` uploaded flat (the portal cannot create
  subdirectories); the app accepts the flat layout.
- Duration target: none; the app exits on its own.

## Timeline

| Time | Event |
| --- | --- |
| 23:05 | run.txt uploaded, app launched |
| 23:07 | log and screenshot pulled after 75 s |

## Telemetry

| Metric | Value |
| --- | --- |
| Duration | ~75 s (app exited on its own) |
| Presented frames | 1 flip |
| Guest threads | 27 |
| Files opened | 83 |
| GPU submits | 116 (`dcb=5/4`, `q=116`) |
| Draws | 1 (quad, indexed) |
| Compute pipelines | 5+ compiled (`a0b87e41`, `99086cd4`, `302d11fa`, `1e8e11d0`, `06d71bb`, `19367542`, `43b8ee5e`) |
| RAM | not printed before the exit; startup was 300 MiB |
| `scheduler.Finish()` / arena / QUAD | not printed (HUD path never runs in this profile) |

## Reached gameplay?

- Verdict: **no**
- Evidence: the run ends at a graphics pipeline creation failure, before any scene frame.

## Failures

| Kind | Detail |
| --- | --- |
| Device loss | none |
| HRESULT | none |
| Assert | `M5 ASSERT vk_graphics_pipeline.cpp:571 kind=assert-msg` |
| Pipeline reject | `graphics create failed result=ErrorOutOfHostMemory (-1) fs_0x00000000b1032a3f,vs_0x000000004f38cbc0` |
| Remaining unsupported imports | `dl+4eHSzUu4#N#O` (sceAjmInitialize), `Q3dyFuwGn64#N#O` (sceAjmModuleRegister) ×2 — answered with 0 |

## Visual symptoms

Screenshot shows the dashboard: the app exited. No frame was ever presented to the guest window.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-volta-ao-dashboard.png`](captures/01-volta-ao-dashboard.png) | after exit | dashboard with the Uncharted tile |

## Logs

- `xbox/.local/logs/19-11-2305/loader.log` (42 870 bytes)
- `xbox/.local/logs/19-11-2305/raw.log` (30 771 bytes, pipeline and GPU markers)
- Key lines: `M5 gnm stats: submit=1/1 flip=1/1 ... draws{rect=0 quad=1 ...}`;
  `M5 pipeline: graphics create failed result=ErrorOutOfHostMemory (-1) fs_0x00000000b1032a3f,vs_0x000000004f38cbc0`

## Conclusion

Two real advances and one wall:

1. `BB_UNRESOLVED=log-fail` carries the guest past the AJM imports; the port then reaches the GPU
   command stream and submits 116 commands with one flip and one indexed quad draw.
2. The next wall is a **graphics pipeline Dozen refuses**. `VK_ERROR_OUT_OF_HOST_MEMORY` from this
   ICD is the documented generic rejection (`KNOWN_ISSUES.md` #4), so the cause is a pipeline state
   Dozen does not accept — not memory pressure.

The M5 diagnostic profile prints "logical VideoOut only; guest renderer not attached", so even a
winning pipeline here would not produce a frame. The normal profile does not link today
(`bbgpu_uwp_*` symbols are compiled out without the M5 flags), which is the other half of the work.

## Next single action

Dump the rejected pipeline's state (the `M5 pso:`/`cso2` markers give the key parts) and compare it
against Dozen's limits — starting with the render target format and blend state of
`fs_b1032a3f`/`vs_4f38cbc0`.
