# Run continue-com-retire-20-11-0004 — Continue with the packet-retire experiment

Folder: `runs/continue-com-retire-20-11-0004/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 00:04 (local) |
| Commit | `190a636` + uncommitted fixes (prepare on console, tess default, title ID) |
| Package | `UnchartedNDCUWP_1.0.26324.237_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_ARENA_PACKET_RETIRE=1` (extra line in `run.txt`) |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Repeat the movie→gameplay path with the packet-scoped arena retirement enabled, and then press
**Continue** on the main menu — the point where the game is expected to fail.

## Timeline

| Time | Event |
| --- | --- |
| 00:00 | press sequence: title → game selection → offline Friend Stats OK → main menu |
| 00:01 | main menu: **Continue (Ambushed)**, Options, Bonuses, New Game, Load Game — 48.8 FPS |
| 00:02 | `cross` on Continue |
| 00:03 | screenshot endpoint returns **HTTP 500**, both processes still alive |
| 00:03 | `loader.log` ends at `M5 wait begin: equeue=GPU NdJob Sync Queue` |

## Telemetry

| Metric | Value |
| --- | --- |
| Main menu FPS | 48.8 (20.48 ms) |
| RAM on the menu | 3 747 / 3 751 MiB |
| FIN / ARENA / QUAD on the menu | 2 / 2 / 37 draws, 222 indices |
| After Continue | no frames; process alive, screenshot 500 |

## Reached gameplay?

- Verdict: **no**
- Evidence: the run stops on Continue. `HTTP 500` from `/ext/screenshot` means the app is not
  presenting; the log's last line is a GPU equeue wait.

## Failures

| Kind | Detail |
| --- | --- |
| Hang on Continue | last line `M5 wait begin: equeue=GPU NdJob Sync Queue tid=4164` |
| Device loss | not reported before the log was pulled |
| Assert | none |
| Access violation | none in this log (the earlier AV happened without the retire flag) |

## Visual symptoms

Nothing new: the menu itself renders correctly. The coin/Bluepoint wedge is not visible in these
captures because the run never reached a loading screen.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-menu-principal.png`](captures/01-menu-principal.png) | menu | main menu with Continue (Ambushed) |
| [`captures/02-menu-principal-2.png`](captures/02-menu-principal-2.png) | menu | same menu, different frame |

## Logs

- The 42 MB `loader.log` of this run was not kept (dominated by `M5 target map note`).

## Conclusion

`BB_ARENA_PACKET_RETIRE=1` did not make Continue work: the app stops presenting and sits in a GPU
wait. It is a different failure from the access violation seen without the flag — a hang instead of
an AV — but still a dead end at the same place.

## Next single action

Gate the `M5 target map note` spam (it hides the interesting lines and costs ~900 writes/s), then
re-run Continue with the flag and pull only the tail, to see whether the retire path is stuck on the
GPU or on the guest.
