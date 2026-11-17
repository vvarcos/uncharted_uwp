# History

A short record of what was achieved, in the order it was achieved, and the decisions that came
out of it. Every entry below corresponds to console evidence; nothing here is a roadmap claim.

## Milestone ladder

| Milestone | Goal | State |
| --- | --- | --- |
| M1 | UWP package that loads and executes the guest image | **done** |
| M2 | Sparse/demand guest memory and the game's own entry point | **done** |
| M3 | Guest runtime services and memory contract on console | **done** |
| M4 | Connected renderer: movies, 3D menu, first real frames | **done** |
| M5 | Gameplay: 24 pipelines, three arenas, 600 s, peak < 5000 MiB | **met on console; copy path still bypassed** |
| M6 | Uncharted 2 and 3 | not started |

M5 is judged as a whole. A real frame, the menu or a finished level load is progress, never a
closed milestone.

## October 2026 — M1: the package runs native code

The UWP package was built with clang-cl, the Store CRT, `WindowsApp.lib` and UCRT winpthread, with
a CoreApplication host providing activation, private configuration, logs and a 30-second
initialisation watchdog. Desktop and Linux builds kept their existing implementations.

Validated on PC UWP and on Series X:

- the package registers, installs and activates;
- imports resolve through `WindowsApp.lib`, the Store CRT or packaged DLLs;
- read-write memory can be made execute-read and a System V x64 function executes and returns the
  expected value;
- the guest image maps 31,082,556 bytes in six segments, with 15,940 relocations and 117 TCB loads
  redirected to the Windows TLS slot;
- the game's own libc initialiser runs and registers its heap API.

The desktop allocator's whole-range reservation failed with error 487 on console because UWP host
allocations already occupied part of the PS4 user interval. M1 deferred that reservation so code
loading could be tested independently. This was a diagnostic milestone, not a memory workaround.

## October 2026 — M2: sparse memory and the original entry

The allocator now reserves guest windows on demand and commits touched 16 KiB guest pages, while
host protection stays at 4 KiB for GPU tracking. Direct-page contents survive unmap/remap; live
aliases are promoted to small shared sections.

All memory contract tests passed on PC UWP and Series X, including protected host copies and mixed
4 KiB protections applied before first touch. Both platforms completed the linked libc and Fios
initialisers and executed the game's actual entry point. The guest maps 2.75 GiB of direct memory
and had committed 107 guest pages (about 1.67 MiB) at the current stop.

The stop was `sceVideoOutOpen`: the graphics backend was not linked yet.

## October 2026 — M3: runtime services and a stutter fix

Guest graphics services, controller APIs, silent audio timing, ATRAC9/MP3/video decoding and the
runtime services were linked into the UWP executable. A 10.7-second opening pause was traced to
file-read buffer ownership checks (4 ms of actual host I/O) and to unmaps holding the region-table
lock for up to 3.4 s each.

`uwp_detach` scanned the entire virtual-page table for a replacement page owner even when only the
mapping being removed remained. Skipping that scan for the last mapping removed 50 long unmaps and
the 11–12 s pauses. The memory contract now covers canonical-owner removal and final-alias removal.

## October 2026 — M4: renderer, movies and menu

The renderer was connected to the host. Runs completed initialisation, entered the game, hid the
splash screen, started the intro loader, audio driver and streaming threads, and rendered the
Sony and Naughty Dog logos, brightness setup, the opening movie and the 3D main menu. Scripted
Cross input advanced setup, New Game and difficulty selection. Single-sample rendering fixed the
menu's striped depth corruption and became the UWP default.

Console rendering required several structural workarounds, all still in place:

- ordinary buffers instead of sparse arenas;
- a logical page table plus buffer descriptors instead of buffer device addresses;
- descriptor sets allocated and updated on push, instead of push descriptors;
- shader permutations returning zero for absent resources;
- rect/quad list expansion without tessellation;
- integer textures read with an explicit texel coordinate, because Dozen is capped at shader
  model 6.4.

## October–November 2026 — M5: partial

### Presentation reaches the game

Frames are presented through the CoreWindow bridge. The GPU history ring initially recorded only
dispatches, because the draw checkpoint was compiled under the wrong conditional; fixing it showed
that a pending submit can contain draws, dispatches and copies together.

### The scratch OOM was fixed

`Runtime::CopyColorAndDepth` reserved 128 MiB per copy. Under `UNCHARTED_UWP` it now sizes the
scratch from the real copy dimensions: the console reported 4500 KiB instead of 131072 KiB for
seven copies, with no allocation failure. That removed one OOM but not the copies.

### The device-hang trigger was isolated

Skipping the colour/depth copies (`BB_SKIP_COPY=1`) removed `DXGI_ERROR_DEVICE_HUNG`, showed the
real scene at 720p and ran for 309 s. Skipping only one class of copy (`=2`, `=3`) still hung, and
mode 2 allocates no scratch at all, so the trigger is the copy path itself rather than a format or
the scratch buffer. The skip is now the default on UWP for diagnosis; `=0` restores the copies.
See [KNOWN_ISSUES](KNOWN_ISSUES.md) for the full list of refuted hypotheses.

### The memory ceiling became the second blocker

With the copies bypassed the run no longer loses the device but dies at the absolute ceiling
(`used=5056 peak=5056 limit=5120 MiB`). A quantised sample showed the growth is in buffers, not
images, and that the decommit path releases nothing. The shader cache was tested as a lever and
did not help. See [MEMORY](MEMORY.md).

### The precompiler was validated

A separate precompile mode waits for the warm-up, saves the pipeline cache and exits before the
guest starts: the console showed `waiting` → `complete`, zero compilations in the log and no
assert. A later seed measured `Pipeline cache: preloaded 133 of 134 pipelines` (194 shaders,
1.5 s) reading 524 files; the one key left out is a deliberate focus-fix shader (`why=6`), not a
corrupt entry. The game path then reuses that cache instead of compiling.

### The HUD and the 600 s gate

The UWP build used to leave the overlay inert because the desktop menu needs SDL and Dear ImGui.
It now draws two lines straight into the swapchain image the presenter already leaves in
`ColorAttachmentOptimal`: one dynamic-rendering pass, one pipeline, one atlas baked once from the
embedded font and one host-visible vertex buffer per image. Cost is 75 KiB and there is no
per-frame allocation. On console the capture reads `RAM 3229/3486/5120 MiB` and
`39.9 FPS 25.08 ms`.

The peak shown there is not a 2 Hz sample: `AppMemoryUsage` is now read on every present and the
running maximum is kept, so the number is authoritative for the ceiling.

Three independent runs then held gameplay for over 600 s each (41 774 / 38 037 / 37 603 frames)
with peaks of 3888 / 3897 / 3837 MiB — more than 1200 MiB of headroom — and no assert, STOP,
`DEVICE_HUNG` or access violation. One run compiled 31 graphics and 17 compute pipelines on
demand, over the 24 required.

Two memory faults were fixed on the way. `GetArena` published only whole 512 MiB pages, so a
demand-sized arena smaller than a page left pointers to a destroyed buffer and repeated the same
cross-page merge (measured: nine merges growing 512 → 1088 MiB); it now publishes both virtual
pages and unpublishes every alias before destroying. And anchoring a tiny access at the 512 MiB
page base produced 320–512 MiB allocations that the Dozen heap refuses long before the app
ceiling; windows are anchored in 64 MiB quanta instead.

## Decisions that are closed

| Decision | Reason |
| --- | --- |
| 5120 MiB ceiling treated as absolute | Crossing it terminates the app; there is no workaround. |
| GPU memory counted 1:1 against the app | Measured on console; the device heap is not the limit. |
| Three 512 MiB arenas retained | Measured console decision; reopening requires new evidence. |
| Arenas host-cached and demand-allocated | Avoids paying 1536 MiB up front. |
| `BB_SKIP_COPY=1` default on UWP | Diagnostic default; the copies are still required for M5. |
| Linear-image readbacks always enabled | Without them, valid asset bytes pair with wrong descriptors. |
| 720p, 60 Hz, two frames queued on console | The only profile that fits and presents reliably today. |
| No tessellation | Dozen has no tessellation shader; lists are emulated. |
