# Run 000 — reconstruction baseline

Folder: `runs/000-reconstrucao/` — no captures; nothing was captured on the original runs.

**Not a run.** This entry exists because the repository's documents mixed menu presentation with
gameplay, and the corrected baseline has to live somewhere with the same authority as a real run.

- Date: 2026-11-19 (written)
- Commit: `190a636` (the state the reconstruction is based on)
- Package: `.1415` (boat scene capture), `.1613` (composite isolation) — older builds, not current
- Run mode: manual Continue, `BB_SKIP_COPY=1`
- Switches: `BB_SKIP_COPY=1`, packet-scoped arena retirement on the `.1415` run
- Evidence: documents only. No log, no capture, no package retained.

## What the documents claim

| Claim | Where |
| --- | --- |
| Three runs held gameplay >600 s | `HISTORY.md`, `KNOWN_ISSUES.md` #1 |
| 41 774 / 38 037 / 37 603 frames, peaks 3 888 / 3 897 / 3 837 MiB | `KNOWN_ISSUES.md` #1 |
| Scene rendered correctly for 5 838 frames at 35–57 FPS | `docs/CONTINUE_LIFETIME.md` |
| Real gameplay: magenta/cyan materials, black geometry, ~3 FPS, 4 411 MiB | `docs/CONTINUE_LIFETIME.md` |

## The correction

41 774 frames over ~696 s is **60 Hz**. At the observed 3–4 FPS, 600 s would be ~2 100 frames.
The frame counts therefore prove a menu (or a static first scene) being presented at 60 Hz, not
gameplay. The same applies to the 5 838-frame / 35–57 FPS run: a rate above 30 FPS has never been
observed in gameplay, so that run was not gameplay either.

User testimony, consistent with the `.1415` capture: gameplay runs at **3–4 FPS** with characters
disappearing, vertices exploding and severe colour corruption.

## What this changes

1. The M5 gate is **open**. `HISTORY.md` and `KNOWN_ISSUES.md` #1 still call it met; they are wrong
   and are corrected.
2. The memory result (3 837–3 897 MiB peak) is a **menu** peak. The only gameplay memory figure is
   4 411 MiB at 3 FPS, from one capture.
3. The `b4fa3a82` composite diagnosis was made on a run that was not gameplay. It remains a real
   stall (the device is lost at that draw), but it is not yet known how it behaves in sustained
   gameplay.

## Next single action

Measure the 3–4 FPS before touching anything else: counters for `scheduler.Finish()`, arena
replacements and QUAD_LIST expansion, shown on the console HUD.
