# Run primeiro-build-depois-da-perda-19-11-2230 — first build after the loss

Folder: `runs/primeiro-build-depois-da-perda-19-11-2230/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-19 22:30 (local) |
| Commit | `190a636` + uncommitted HUD/telemetry changes |
| Package | `UnchartedNDCUWP_1.0.26324.128_x64__70t7zv47487b6` |
| Build stamp | `Build Nov 19 2026 22:28:18` |
| Run mode (`run.txt` line 1) | none — no `run.txt` uploaded |
| Switches | none |
| Dump / patch | `E:\dumps\CUSA02320` |

## Intent

Prove that the recovered tree still builds, installs, boots and presents on the console with the
recovered Dozen driver — nothing about gameplay.

## Setup

- Console: Xbox Series S, `26100.9662.amd64fre.xb_flt_2610ge.261104-2200`
- Toolchain: clang-cl 23.1.4, Windows SDK 10.0.26100, CMake/Ninja from MSYS2/Strawberry, VS 2022
- Driver: `vulkan_dzn.dll` recovered from the `Cosmo/gow3_xbox` release
  `alpha-1.0.26299.2139`; SHA-256 `915642B9…D2E3`, the same hash recorded in `docs/DOZEN.md`
- Signing: new self-signed `CN=uncharted-uwp` key (`xbox/.local/signing.pfx`); the old key was lost
  with the deleted tree, so the publisher hash changed from `pa0j4mwgr1yfy` to `70t7zv47487b6`
- Cleaned `LocalState/raw.log` before the run: not applicable (fresh install)
- Scripted input (`pad.txt`): not used
- Duration target: none

## Timeline

| Time | Event |
| --- | --- |
| 22:28:18 | build `uncharted_uwp.exe` + `bbgpu_dll.dll` |
| 22:28:52 | signed package `1.0.26324.128` |
| 22:29:0x | installed on `series-s`; install returned `installed: true` |
| 22:29:1x | launched, PID 1264, `running_observed: true` |
| 22:29:32 | screenshot: solid blue clear frame |
| 22:29:51 | `loader.log` pulled |

## Telemetry

| Metric | Value |
| --- | --- |
| Duration | ~60 s until checked |
| Presented frames | 1 (graphics probe clear) |
| Mean FPS | not measured (no guest frames) |
| RAM current / peak | 332 / 332 MiB (`UWP startup memory`) |
| Private working set | 25.5 MiB |
| Graphics pipelines compiled | 0 |
| Compute pipelines compiled | 0 |
| Arena window replacements | 0 |
| `scheduler.Finish()` calls | 0 |
| QUAD_LIST draws / indices expanded | 0 / 0 |

## Reached gameplay?

- Verdict: **no**
- Evidence: the log ends at `M2 READY: graphics host complete; select M3 stage, memory, init or entry
  explicitly.` The guest is never started because there is no `run.txt` selecting a mode.

## Failures

| Kind | Detail |
| --- | --- |
| Device loss | none |
| HRESULT | none |
| Assert | none |
| Access violation | none |
| Last scheduler tick | not applicable |

## Visual symptoms

Solid blue frame: the CoreWindow clear from the M2 graphics probe. The HUD does not appear because
the probe path presents one clear image without going through `BbOverlay::Render`.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-splash-azul.png`](captures/01-splash-azul.png) | boot | solid blue clear, no HUD, no guest |

## Logs

- `xbox/.local/logs/19-11-2230-primeiro-build-loader.log` (5 488 bytes, full startup)
- Key lines: Dozen reports `Microsoft Direct3D12 (SraKmd_arden), Vulkan 1.2`;
  `bufferDeviceAddress=0 sparseBinding=0`; CoreWindow bridge `1280x720`, `3904 KiB` shared image;
  `Graphics probe: Vulkan clear -> shared D3D12 image -> CoreWindow PRESENTED`

## Conclusion

The tree recovers. The same binary driver used in the earlier experiments loads, the device is
`SraKmd_arden` on D3D12, and the app presents through the CoreWindow bridge. Nothing about
rendering correctness or frame rate is proven by this run.

## Next single action

Upload `run.txt` with the mode that starts the guest, using the steps the user has not given yet.
