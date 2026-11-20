# Run moeda-sem-o-fix-instalado-20-11-0004 — coin still wedged because the fix was not installed

Folder: `runs/moeda-sem-o-fix-instalado-20-11-0004/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 00:04 (local) |
| Commit | `190a636` + uncommitted fixes, **including** the QUAD_LIST winding correction |
| Package | `UnchartedNDCUWP_1.0.26324.237_x64__70t7zv47487b6` — **built before the winding fix** |
| Run mode | `entry-unresolved` |
| Switches | `BB_ARENA_PACKET_RETIRE=1` |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Check whether the QUAD_LIST winding correction removes the black wedge on the loading coin.

## What went wrong with the procedure

The renderer was rebuilt with the fix, but the package step was skipped: `package.ps1` was not run
again, so `xbox/.local/latest-package.txt` still pointed at the previous `.appx` (version
`1.0.26324.237`, built before the correction). The install reported success for that same version,
which is how the mistake went unnoticed — the version did not change between builds.

Consequence: **this run proves nothing about the fix.** The coin wedge is expected here because the
tested binary does not contain the change.

## Timeline

| Time | Event |
| --- | --- |
| 00:03 | launch, press sequence to the main menu |
| 00:03 | three `down` presses (Continue → New Game) and `cross` |
| 00:04 | loading coin on screen, 55.5 FPS, 18.01 ms, RAM 3 942 MiB |

## Telemetry

| Metric | Value |
| --- | --- |
| Loading screen FPS | 55.5 (18.01 ms) |
| RAM | 3 942 / 3 942 MiB |
| FIN / ARENA / QUAD | 2 / 2 / 28 draws, 168 indices |

## Reached gameplay?

- Verdict: **loading screen only**

## Failures

| Kind | Detail |
| --- | --- |
| Device loss | none |
| Assert | none |
| Process exit | none observed during the captures |

## Visual symptoms

The spinning loading coin keeps the black triangular wedge at the bottom: one half of the quad is
missing. With the pre-fix binary this is the known defect, not a new regression.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-moeda-com-cunha.png`](captures/01-moeda-com-cunha.png) | loading | coin with the black wedge, HUD counters visible |

## Conclusion

Procedure failure, not a code failure: the installed binary predates the fix. The lesson is recorded
in `GOAL.md`'s protocol — **rebuild, repackage and confirm the new version number** before claiming
a run tested a change. A version that repeats between builds is a false signal.

## Next single action

Install `1.0.26324.303` (built with the winding fix), repeat the New Game path, and compare the coin
frame against this capture.
