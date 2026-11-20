# Plan: from "renders" to "playable"

This is the ordered backlog behind [`GOAL.md`](GOAL.md). The order is fixed: **visual correctness
first**, then frame time, then duration. Each item has its own acceptance evidence; nothing moves
forward on a guess.

## How every item is executed

1. Read the code path and write down the hypothesis in the run entry before changing anything.
2. Implement the smallest change that tests the hypothesis.
3. `build.ps1` → `package.ps1` (it refuses a stale renderer) → install → **remove every other
   package version** → confirm the version on the console.
4. Reach the state with `xbox/scripts/pad.ps1` (D-pad and Cross only; sticks need `-Stick`).
5. Capture with `xbox-screenshot.ps1`, pull `loader.log`.
6. **Watch the run until the app exits** (`xbox/scripts/watch.ps1`): a frame every few seconds, so a
   crash at 40 s is not mistaken for a loading screen at 30 s. The entry's timeline comes from those
   frames.
7. Write the entry, update [`runs/INDEX.md`](runs/INDEX.md), commit and push.
8. If the hypothesis fails **twice**, stop and move to the next item; record the dead end.

Current build: `1.0.26324.436` and later. Mode: `entry-unresolved`. New Game is the path to the
level; Continue shares the same load.

---

## Phase 2 — Visual correctness (current)

The rule for this phase: **a screenshot of the boat scene with plausible colours, no missing
geometry, no duplicated text and Xbox prompts**. No performance work until that holds.

### Where the last session stopped (2026-11-20, 02:00)

Two facts, from `runs/copias-ligadas-gpu-stall-20-11-0152/` and
`runs/composite-pulado-tela-preta-20-11-0156/`:

- **Copies off** (`BB_SKIP_COPY=1`, the current default): the game plays, textures are magenta.
- **Copies on** (`=0`): the access violation is gone (the arena pin fixed it), but the load stops in a
  GPU stall after 25 632 submits. Skipping the composite shader is not a workaround — it is the draw
  that puts the scene on screen, so the result is a black screen at 57 FPS.

So the session starts with one measurement, not a guess: with copies on, dump the submit that stalls
(`BB_GPU_HISTORY=1` is already on) and say whether the composite samples an image the copies wrote or
a different one. That answer decides between fixing the copy path and fixing the magenta without the
copies.

### 2.1 Colour/depth copies on by default

- Hypothesis: magenta/cyan is the composite sampling an image the colour/depth copies never filled.
  `BB_SKIP_COPY` defaults to `'1'` in `vk_runtime.cpp`, so the copies are off in every gameplay run.
- Change: `BB_SKIP_COPY=0` as the UWP default (once the run proves it), keep `=1` only as a
  diagnostic switch.
- Accept: boat scene in plausible colours, app alive, RAM under 4 600 MiB.
- Fallback (if colours stay wrong): 2.2.

### 2.2 Texture format and descriptor audit

- Steps: log the sampled image's `vk::Format` and `GetSupportedFormat()` substitution per draw for
  one frame; compare with the guest's GNF format. Check `texture_cache` for format fallbacks that
  silently pick a different layout.
- Accept: the format the shader samples is the format the asset was uploaded with, in the log.
- Fallback: 2.3.

### 2.3 Null/absent descriptors

- Steps: the port satisfies missing bindings with a null descriptor (`vk_rasterizer.cpp:2876`,
  `:3620`). Log every such substitution with the shader hash and the binding index.
- Change: bind a 1×1 neutral texture instead of a null descriptor, so a missing binding is grey, not
  black and not garbage.
- Accept: no black blocks; the log lists every substitution.

### 2.4 Prompt textures

- Steps: verify the Xbox set on title, tutorial, QTE and pause prompts; the doubled text came from
  two prompt layers drawing at once.
- Accept: single prompt per action, A/B/X/Y glyphs, no overlap.
- Status: `BB_BUTTON_PROMPTS=xbox` is on and the tutorial prompt already reads LT/RT, single line.

### 2.5 Characters and skinning

- Steps: capture a frame with Nate on screen at close range; check limbs, hair, cloth.
- Accept: no exploded vertices, no missing body parts, no stretched polygons.
- Evidence: the earlier coin/Bluepoint wedge is gone; this item is about skinned meshes.

### 2.6 Shadows, lighting, decals

- Steps: capture two scenes with different lighting (boat deck, interior).
- Accept: shadows land where geometry says they should; no full-screen dark or bright bands.

---

## Phase 3 — Frame time

Only after Phase 2 holds.

### 3.1 Attribute the frame (292 ms today)

- Instrument three numbers per frame: time in `GetArena`/arena rebuild, time recording draws, time
  waiting on the GPU. They must roughly sum to the frame time.
- Accept: the split is in the HUD and in the log; no number is "other".

### 3.2 Arena churn

- Fact: `ARENA 66` replacements per frame, now without `Finish()`.
- Steps: count replacements by request size; cache windows by size class; rebuild only when the
  request truly falls outside the window.
- Accept: replacements per frame drop by an order of magnitude with the same picture.

### 3.3 Draw submission and GPU wait

- If the split says GPU wait dominates, measure per pass (colour, depth, composite) before touching
  anything.
- If recording dominates, the per-draw work is the target: descriptor sets, index expansion, push
  data.
- Accept: the dominant term is named in a run entry before a change is attempted.

### 3.4 Targets

- 10 FPS with the same picture, then 30 FPS mean and 95th percentile frame time under 40 ms.

---

## Phase 4 — Duration and memory

### 4.1 Short runs first

- 60 s, then 300 s, then 600 s of gameplay with copies on. Each is its own run entry with the peak
  memory and the last HUD sample.

### 4.2 Memory headroom

- Keep the peak under 4 600 MiB; the ceiling is 5 120 MiB and crossing it kills the process.

---

## Phase 5 — Gate

- One run with every acceptance criterion of `GOAL.md`, two gameplay captures from different scenes.
- Then update `README.md`, `HISTORY.md`, `KNOWN_ISSUES.md` and `MEMORY.md` from that entry.

---

## Decisions already made (do not reopen)

| Decision | Why |
| --- | --- |
| Visual correctness before performance | A fast wrong picture is worthless, and colour bugs change what the profiler measures. |
| Copies on by default if 2.1 passes | The composite needs them; the crash that forced them off is fixed by the arena pin. |
| Null descriptors become neutral textures, not errors | A missing binding must degrade, not paint black or crash. |
| D-pad for scripted input | A held stick leaves the character walking after the agent stops. |
| Never destroy an arena inline | The pin/retire path owns it; inline destruction is the bug that ended the level load. |
| One package version installed at a time | Two versions make the launcher pick the wrong one and void the run. |
| No change without a run entry | The repository is the memory; chat is not. |
