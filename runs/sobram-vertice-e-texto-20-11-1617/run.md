# Run sobram-vertice-e-texto-20-11-1617 — the two visual defects left, from the user's screen

Folder: `runs/sobram-vertice-e-texto-20-11-1617/` — the user's capture is described here (not stored: it
was sent as a picture, and it must be saved to `captures/` on the next occurrence).

| Field | Value |
| --- | --- |
| Date | 2026-11-20 16:17 (local) |
| Commit | `26f55c9` |
| Package | `UnchartedNDCUWP_1.0.26324.1809_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` (copies skipped) |
| Overlay | `3.4 FPS 294.53 ms`, `FIN 0`, `QUAD 34 idx 20 40`, RAM `4591/5120 MiB` |

## What the user reports

After the level-load and copy work, only two visual defects remain:

1. **A vertex now and then explodes**: geometry stretches across the scene (the capture is a nearly
   black frame split by a huge dark wedge — the same class of stretched triangle already seen on the
   coin, but occasional and on world geometry).
2. **Duplicated text**: the tutorial line is drawn twice, offset, in two weights —
   `Press Ⓑ to Take cover when Next to an object` over `Press Ⓑ to take cover when next to an object`.

Everything else the user checked reads correctly: HUD, gun, prompt glyph (the Ⓑ is the Xbox button),
RAM under the ceiling.

## Why these two are separate problems

- The vertex explosion is intermittent and appears on world geometry, so it is a *vertex input* defect:
  a vertex buffer or an index/instance stream read before the GPU finished writing it, or a bone
  matrix that was never uploaded. The port already has the tools for that class
  (`BB_VERTEX_CHECK`, `BB_CLOBBER_CHECK`, `BB_BONE_CHECK`, `BB_FORCE_VB_UPLOAD`), and `KNOWN_ISSUES.md`
  lists "vertices exploding" separately from the colours.
- The duplicated text is deterministic and belongs to the prompt layer: `button_prompts.cpp` redraws the
  prompt sheet with a host font, and the capture shows the host text *on top of* the guest text at a
  different size. `PLAN.md` 2.4 already recorded that "the doubled text came from two prompt layers
  drawing at once"; this capture is that state.

## Next actions

1. `BB_VERTEX_CHECK=1` in a run that reaches gameplay: the check reports a vertex read outside the
   buffer or a NaN/Inf position, with the draw hash — that names the pass that explodes.
2. Prompt duplication: log every image `ReplaceButtonPrompts` rewrites (address, size, mip count) and
   compare with the two text layers in the capture; if the replacement covers only one layer, the other
   keeps the guest text. The fix is to make the replacement cover both or to suppress the duplicate
   layer draw.
