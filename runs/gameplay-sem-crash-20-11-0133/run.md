# Run gameplay-sem-crash-20-11-0133 — level load survives, gameplay reached

Folder: `runs/gameplay-sem-crash-20-11-0133/` — screenshots of this run go to `captures/` beside this file.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 01:33 (local) |
| Commit | `f52b8a6` |
| Package | `UnchartedNDCUWP_1.0.26324.426_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | none; input through `xbox/scripts/pad.ps1` (`cross`/`down`, D-pad only) |
| Dump | `E:\dumps\CUSA02320` |

## Intent

Validate the Phase 1 fix (arena pinning) on the level load: does the app survive the loading coin?

## Result

**Yes.** The log contains **no** `M5 bound buffer destroyed` and **no** `M5 av-scan`, and the app is
alive in gameplay: Drake and Elena on the boat deck, ammo HUD, tutorial prompt. The arena retirement
now reports `M5 arena retire: destroyed=1 pinned=0`, i.e. it only destroys when nothing references
the handle.

## Telemetry

| Metric | Value |
| --- | --- |
| Frame rate | 0.0–2.2 FPS (438–502 ms per frame) |
| `FIN` | 62–63 calls, 511–645 ms per frame |
| `ARENA` | 62–63 replacements per frame |
| QUAD / indices | 27 draws, 164 indices |
| RAM | 4 341–4 466 MiB current, 4 449–4 623 MiB peak |
| Process | alive, ~3.9 GB working set |

## Failures

| Kind | Detail |
| --- | --- |
| Access violation | **gone** on the level load |
| Device loss | none |
| Assert | none |

## Visual symptoms

- Whole scene in magenta/cyan/purple, black blocks where textures belong (Phase 2).
- The tutorial prompt is drawn twice, overlapping ("Press ○ to take cover…").
- Prompts still use PlayStation glyphs on an Xbox pad.

## Captures

| File | Scene | What it shows |
| --- | --- | --- |
| [`captures/01-gameplay-conves.png`](captures/01-gameplay-conves.png) | gameplay | boat deck, HUD `FIN 62 511ms ARENA 62`, RAM 4 466 MiB |

## Conclusion

Phase 1 is closed: pinning the arena while a descriptor write holds its handle removes the
level-load access violation, and the app stays alive in gameplay. The pin costs nothing measurable at
this frame rate, and the retirement path no longer calls `Finish()` when everything is pinned.

The 62 arena replacements per frame, each still paying `scheduler.Finish()` inside `GetArena`, are now
the single biggest cost (511 ms of a 502 ms frame). That is Phase 3's first target.

## Next single action

Remove the `Finish()` from `GetArena` (the pin makes it redundant) and measure the frame time.
