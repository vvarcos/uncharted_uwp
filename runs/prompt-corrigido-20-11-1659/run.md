# Run prompt-corrigido-20-11-1659 — one prompt line, correct colours, 5.4 FPS

Folder: `runs/prompt-corrigido-20-11-1659/` — captures `01-prompt-unico-cores-ok-65s.png` and
`02-71s.png`.

| Field | Value |
| --- | --- |
| Date | 2026-11-20 16:59 (local) |
| Commit | `74efba8` (prompt fix) + `60c9be9` (vertex suspect log) |
| Package | `UnchartedNDCUWP_1.0.26324.1951_x64__70t7zv47487b6` |
| Run mode | `entry-unresolved` |
| Switches | none |

## Result

Clean frame of the boat scene:

- **one** prompt line, correct Xbox glyph: `Press Ⓑ to take cover when next to an object`;
- colours correct on deck, crates, characters, sea and clouds;
- `5.4 FPS 185.43 ms`, RAM `4547/4681/5120 MiB`;
- the app ran the whole watch (the exit came from the watch limit, not a crash).

The preceding run with `BB_VERTEX_CHECK=1` on the same package showed the magenta/rainbow scene again.
The only difference was the check, so **the vertex check perturbs what it measures**: it reads guest
memory out of band, the port's page tracking sees those reads, and images the GPU had written are
re-uploaded from stale guest bytes. The colour diagnostics must therefore run with the check off, and
the vertex work needs a check that does not touch guest memory (validate the stream on the GPU side, or
compare a hash instead of reading the range).

## State

| Item | State |
| --- | --- |
| Colours | correct in the normal configuration |
| Prompt text | single line, correct glyph (`74efba8`) |
| Vertex explosion | still open; `M5 vertex suspect` is in place but the check cannot run with the scene correct |
