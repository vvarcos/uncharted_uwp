# Run pin-de-arena-no-carregamento-20-11-0121 — the pin build never got a clean run

Folder: `runs/pin-de-arena-no-carregamento-20-11-0121/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 01:21 (local) |
| Commit | uncommitted at the time; landed as `1f4b94f` |
| Package | `1.0.26324.420` and `1.0.26324.421` both installed |
| Run mode | `entry-unresolved` |
| Switches | none |

## Intent

Check whether pinning an arena while a descriptor write holds its handle removes the level-load
access violation (`M5 bound buffer destroyed` → `M5 av-scan: addr=0x10`).

## Timeline

| Time | Event |
| --- | --- |
| 01:21 | 421 installed over 420; the launcher resolved the first match, 420 |
| 01:21 | launch "accepted but not observed running"; every later command failed with "App selection must match exactly one installed package" |
| 01:22 | the pulled log was the previous run's (same 19 559 464 bytes, same AV) |

## Reached gameplay?

- Verdict: **no run** — the app never started under the intended package.

## Failures

| Kind | Detail |
| --- | --- |
| Procedure | two versions installed; `-App` matched both; no launch |
| Real crash | the old log still shows the AV; it says nothing about the pin build |

## Conclusion

Nothing was measured. The lesson is procedural and now enforced by hand: after installing, remove
every other version and resolve the package name **from the console**, not from a file name — the
`_x64_x64_` failure earlier came from building the name by hand.

`1.0.26324.426` (commit `1f4b94f`) is the build that carries the pin change together with the
controller fix, and it is the only package installed.

## Next single action

Run the New Game path on 426 with the pad script and check the log for `M5 bound buffer destroyed`
and `M5 av-scan`: absent means the pin works.
