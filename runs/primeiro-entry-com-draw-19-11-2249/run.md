# Run primeiro-entry-com-draw-19-11-2249 — entry mode without the prepared image

Folder: `runs/primeiro-entry-com-draw-19-11-2249/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-19 22:49 (local) |
| Commit | `190a636` + uncommitted HUD/telemetry changes |
| Package | `UnchartedNDCUWP_1.0.26324.128_x64__70t7zv47487b6` |
| Build stamp | `Build Nov 19 2026 22:28:18` |
| Run mode (`run.txt` line 1) | `entry-realgnm-draw` (rewritten to `entry` by `main.cpp`) |
| Switches | `BB_SKIP_COPY=1` |
| Dump / patch | `E:\dumps\CUSA02320` |

## Intent

First attempt to start the guest: does the recovered build reach the game with the documented run
mode, using only the dump already on the console?

## Setup

- Console: Xbox Series S, `26100.9662.amd64fre.xb_flt_2610ge.261104-2200`
- `run.txt` uploaded to `LocalState/run.txt` (3 lines: mode, dump, `BB_SKIP_COPY=1`)
- `LocalState/Prepared`: absent — the prepare step had never run on this install
- Scripted input (`pad.txt`): not used

## Timeline

| Time | Event |
| --- | --- |
| 22:49 | `run.txt` uploaded, app launched, PID 7004 |
| 22:52:12 | screenshot taken after 120 s |
| 22:52:28 | `loader.log`, `raw.log`, `errors.log` pulled |

## Telemetry

| Metric | Value |
| --- | --- |
| Duration | ~120 s |
| Presented frames | 0 |
| Mean FPS | — |
| RAM current / peak | 317 / 317 MiB |
| Graphics pipelines compiled | 0 |
| Compute pipelines compiled | 0 |
| Arena window replacements | 0 |
| `scheduler.Finish()` calls | 0 |
| QUAD_LIST draws / indices expanded | 0 / 0 |

## Reached gameplay?

- Verdict: **no**
- Evidence: `M3 FAILED: Prepared/boot-linked.bin or content.bin is missing`, preceded by
  `M5 guest image: boot=MISSING (0 bytes) content=MISSING (0 bytes)`.

## Failures

| Kind | Detail |
| --- | --- |
| Device loss | none |
| HRESULT | none |
| Assert | none (a clean `M3 FAILED` return, not an assert) |
| Access violation | none |
| Last scheduler tick | not applicable |

## Visual symptoms

Solid black window: the app never presents a guest frame, so the swapchain shows the cleared image
and the HUD never runs.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-tela-preta-m3-falhou.png`](captures/01-tela-preta-m3-falhou.png) | failure | solid black, no HUD, no guest frame |

## Logs

- `xbox/.local/logs/19-11-2249/loader.log` (1 751 bytes)
- `xbox/.local/logs/19-11-2249/raw.log` (680 bytes)
- Key lines: `M5 diagnostic: invoking Uncharted U1 controlled guest entry with logical VideoOut, no
  guest renderer`; `M5 guest image: boot=MISSING (0 bytes) content=MISSING (0 bytes)`;
  `M3 FAILED`; `UWP startup memory: 317/5120 MiB`

## Conclusion

The run mode is fine; the install is missing the prepared guest image. `LocalState/Prepared` is
created by the app at startup but never populated, because the BBPROBE5 preparation is a PC step
(`prepare-game.ps1`) whose outputs must be uploaded. This is the step the earlier notes called
"the prepare".

The dump on the console is complete (`eboot.bin`, `big2`, `big3`, `sce_sys/param.sfo`, `u1data`,
`u2data`, `u3data` all found), so the inputs exist; they are just not in `LocalState`.

## Next single action

Run the documented `stage` mode to copy the four preparation inputs into `LocalState/Stage`, pull
them with Device Portal, prepare on the PC, and upload `boot-linked.bin` plus `content.bin`.
