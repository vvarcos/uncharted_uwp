# Run indices-pelo-buffer-cache-20-11-0055 — index reads through the buffer cache

Folder: `runs/indices-pelo-buffer-cache-20-11-0055/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 00:55 (local) |
| Commit | `33bae9a` + local change |
| Package | `UnchartedNDCUWP_1.0.26324.352_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | none |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Reach the loading coin after changing the QUAD_LIST expansion to read the guest indices through the
buffer cache (`InvalidateMemory` + `ObtainBuffer`) instead of the raw guest pointer.

## Timeline

| Time | Event |
| --- | --- |
| 00:55 | launch, title, game selection, Friend Stats OK |
| 00:55 | `cross` on **Continue** |
| 00:55 | app exits; every capture after that is the dashboard |

## Telemetry

| Metric | Value |
| --- | --- |
| Frames | none from the game after Continue |
| Log | 12.6 MB, ends in an access violation |

## Failures

| Kind | Detail |
| --- | --- |
| Access violation | `M5 av-scan: addr=0x10 access=0 rip=0x7ffd54cfe39b` |
| Preceded by | `M5 bound buffer destroyed: guest=0x1100000000 offset=6751632 size=252` |
| Cause | the known arena lifetime bug on Continue, unchanged by this build |

## Reached gameplay?

- Verdict: **no** — the run died on Continue, before any loading screen.

## Captures

None: all frames are the dashboard.

## Conclusion

The change was not evaluated: Continue crashes the process before the coin, exactly as
`docs/CONTINUE_LIFETIME.md` describes. The coin must be reached through **New Game** (menu → New
Game → offline Friend Stats → Campaign → difficulty → loading), which is the path that produced the
coin captures earlier.

## Next single action

Reach the coin through New Game on `1.0.26324.352` and compare with
`runs/moeda-com-renderer-novo-r0-20-11-0046/captures/01-moeda-cunha-R0.png`.
