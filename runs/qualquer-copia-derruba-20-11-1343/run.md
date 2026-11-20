# Run qualquer-copia-derruba-20-11-1343 — any copy submission kills the device

Folder: `runs/qualquer-copia-derruba-20-11-1343/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 13:43 (local) |
| Commit | `032bf34` |
| Package | `UnchartedNDCUWP_1.0.26324.1640_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | `BB_SKIP_COPY=0`, `BB_COPY_SKIP_2K=1` (only the 1280x720 R32<->D32 copies active) |
| Watch | `watch.ps1 -Interval 5 -MaxMinutes 3` |

## Intent

Split the copy failure by size: keep the 1280x720 depth/colour conversions and drop the
2048x2048 depth copies.

## Result

The app exited about 18 s into the load, exactly like every other copy configuration.

Combined with the earlier runs, the matrix is now:

| Configuration | Copies active | Outcome |
| --- | --- | --- |
| default (`BB_SKIP_COPY=1`) | none | plays (magenta) |
| `=0` | all | device lost |
| `=2` | same-format only | device lost |
| `=3` | cross-format only | device lost |
| `=0`, no staging | all, `CopyImage` path | device lost |
| `=0`, render backend | all, full-screen draw | device lost |
| `=0`, skip 2K | 1280x720 R32<->D32 only | device lost |

## The symptom is in the report

`GPU stall: scheduler … timeline …: counter_result=-4` — `VK_ERROR_DEVICE_LOST`. The device is
already gone when the report runs; the "stall" is a wait on a dead device.

## Conclusion

**Any** submitted copy kills the device, whatever the mechanism, format or size. That is not a
transfer bug: it is the submission that carries the copy. The most likely shape is the wait list —
one scheduler waits on a timeline (`…72F0=1`) that never reaches 1, the GPU waits forever, the OS
resets it, and every later query returns `DEVICE_LOST`.

## Next single action

Instrument the submit that carries a copy: print its wait list and the scheduler that owns each
waited semaphore, then compare with the same submit when copies are skipped. The fix is in that
dependency, not in the copy code.
