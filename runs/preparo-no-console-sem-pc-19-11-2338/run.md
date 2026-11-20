# Run preparo-no-console-sem-pc-19-11-2338 — preparation on the console, first gameplay frames

Folder: `runs/preparo-no-console-sem-pc-19-11-2338/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-19 23:38 (local) |
| Commit | `190a636` + graphical fixes (no commit yet) |
| Package | `UnchartedNDCUWP_1.0.26324.237_x64__70t7zv47487b6` |
| Build stamp | `Build Nov 19 2026 23:37` |
| Run mode (`run.txt` line 1) | `entry-unresolved` |
| Switches | none in `run.txt`; the mode sets `BB_REAL_GNM=1`, `BB_DRAW=1`, `BB_NO_AUX_TESS=1`, `BB_UNRESOLVED=log-fail`, `BB_VIDEOOUT_DRIVER=1`, `BB_PAD_FILE=LocalState\pad.txt` |
| Dump / patch | `E:\dumps\CUSA02320` |

## Intent

Prove the two things this session changed: the package prepares the guest image on the console
(no PC step), and the reworked QUAD_LIST path lets the game get past the menus into a real scene.

## Setup

- `LocalState/boot-linked.bin`, `content.bin`, `loader.log` and `raw.log` were **deleted** before
  launch, so the image had to be produced on the console.
- Scripted input: `LocalState/pad.txt` with `cross` (held ~3 s), released with `none`.
  On Xbox the physical A button is Cross (`runtime_pad.c` action table).
- Earlier runs used `entry-realgnm-draw`, which does **not** set `BB_PAD_FILE`; that is why the
  input did nothing until the mode was changed.

## Timeline

| Time | Event |
| --- | --- |
| 23:38 | package installed, prepared files deleted, `run.txt` uploaded, launched |
| 23:39 | title screen ("Press ✕ to Begin"), RAM 3 618 / peak 3 746 MiB, 25.9 FPS |
| 23:40 | first `cross` press: the prompt dims (input reaches the guest) |
| 23:45 | relaunch in `entry-unresolved`, title screen again, 23.0 FPS |
| 23:45 | `cross`: game selection ("Start Uncharted: Drake's Fortune Remastered"), 33.1 FPS |
| 23:46 | `cross` ×3: offline Friend Stats dialog, 38.3 FPS |
| 23:47 | `cross` ×4: **gameplay scene** — night jungle, 57.3 FPS, 3 975 / 4 040 MiB |
| 23:48 | gameplay scene, stone ruins, 53.1 FPS, 3 992 / 4 040 MiB |

## Telemetry

| Metric | Value |
| --- | --- |
| On-console prepare | `M3 prepare: 21349372-byte image, 479 imports, 21330 relocations, fs->gs patched=1953` |
| Menu FPS | 23–38 FPS |
| Gameplay FPS | **53–57 FPS** (sampled) |
| Frame time in gameplay | 17.5–18.8 ms |
| RAM current / peak | 3 992 / **4 040** MiB of 5 120 |
| `scheduler.Finish()` in gameplay | 0–1 per frame, 0–1 ms |
| Arena replacements | 0–1 per frame |
| QUAD_LIST draws | 9 per frame, 54 indices |
| Graphics pipelines | not counted in this build's log tail |

## Reached gameplay?

- Verdict: **cutscene in this run; gameplay reached on the following attempt (user capture)**
- Evidence: the opening movie (night jungle, ruins, the boat with Nate and Elena) and its pause menu,
  which offers "Skip Movie". Skipping it goes to the loading coin, then gameplay.
- Gameplay capture (user, same build): Elena walking on the boat deck with the prompt "Press L2 to
  aim, press R2 to shoot", ammo HUD, and the HUD reading **2.5 FPS, 404.20 ms, FIN 11 / 1656 ms,
  ARENA 11, QUAD 40**, RAM 4 524 / 4 597 MiB.
- Artifacts in that capture: the whole scene in magenta/cyan/purple, and a large **black triangle**
  covering the lower centre of the screen — the same wedge seen on the loading coin and at the end
  of the Bluepoint logo.

The failure at the movie→gameplay transition in this run (before the retry with
`BB_ARENA_PACKET_RETIRE=1`) was the arena lifetime access violation:

```text
M5 bound buffer destroyed: guest=0x1100000000 offset=18582704 size=32
M5 bound buffer destroyed: guest=0x1100000000 offset=4456912 size=252
M5 av-scan: addr=0x10 access=0 rip=0x7ffd54cfe39b ... found: none in self module
```

## Failures

| Kind | Detail |
| --- | --- |
| Device loss | none observed |
| HRESULT | none |
| Assert | none |
| Access violation | none |

## Visual symptoms

Characters render correctly in the movie (Nate and Elena on the boat), with materials, cloth and
hair intact — the skinning path works. No black triangle and no missing geometry in these frames.
Gameplay itself was never reached, so the gameplay-specific defects remain untested.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-titulo-uncharted.png`](captures/01-titulo-uncharted.png) | title | collection title screen with HUD |
| [`captures/02-menu-drake-fortune.png`](captures/02-menu-drake-fortune.png) | menu | game selection, 33.1 FPS |
| [`captures/03-friend-stats-offline.png`](captures/03-friend-stats-offline.png) | menu | offline Friend Stats dialog |
| [`captures/04-gameplay-selva.png`](captures/04-gameplay-selva.png) | cutscene | night jungle, 57.3 FPS, 4 040 MiB peak |
| [`captures/05-gameplay-ruinas.png`](captures/05-gameplay-ruinas.png) | cutscene | stone ruins, 53.1 FPS |

## Logs

- `xbox/.local/logs/19-11-2338/loader.log` (9.7 MB for the first ~90 s)
- The later 42 MB download was not kept: it is dominated by `M5 target map note`, ~900 lines/s.

## Conclusion

The two session changes hold on the console:

1. **No PC step.** With both prepared files deleted, the app produced them itself, byte-identical to
   the Python pipeline, and started the guest.
2. **QUAD_LIST no longer kills the pipeline.** The movie renders with characters; the earlier
   `ErrorOutOfHostMemory` at `vk_graphics_pipeline.cpp:571` did not recur.

The frame rate in the cutscene is 53–58 FPS, and the per-frame counters show `Finish()` and arena
replacements are not the cost (0–2 ms and 0–2 each). What stops the run is the known arena lifetime
bug, hit at the movie→gameplay transition: two buffers of arena `0x1100000000` are destroyed and the
driver then faults at `0x10`. The documented experiment for exactly this is
`BB_ARENA_PACKET_RETIRE=1`, which this run did not set.

Also found: the diagnostic `M5 target map note` prints ~900 lines/s (42 MB of log in ~20 minutes)
and should be gated; and `entry-realgnm-draw` should either set `BB_PAD_FILE` too or document that
it does not.

## Next single action

Repeat the same sequence with `BB_ARENA_PACKET_RETIRE=1` and check whether "Skip Movie" reaches the
first playable moment instead of an access violation.
