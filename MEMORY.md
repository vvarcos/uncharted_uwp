# Memory

Memory is the binding constraint of this port. This document records the budget, what was measured,
and which levers were already ruled out. It is the reference for any change that claims to help.

## 1. The ceiling is absolute

The console gives a UWP app **5120 MiB**. Crossing it by any amount terminates the process; there
is no graceful degradation and no workaround. The device heap is not the limit — the app budget is.
The app's own accounting refuses allocations at roughly 4919 MiB, and the OS kills the process
shortly after if that is ignored.

The 5120 MiB is **shared** between three consumers:

- the application/emulator itself (code, caches, runtime bookkeeping);
- the guest game's memory (heaps, images, buffers);
- GPU memory — every MiB the GPU reserves counts **1:1** against the same budget.

The buffer/cache arena alone is 1024 MiB of that budget. Planning a change that "only" costs
100 MiB of GPU memory costs the game 100 MiB of headroom.

## 2. Measured baseline

| Phase | App memory |
| --- | ---: |
| Boot, before the renderer is created | ~1063 MiB |
| Renderer created and bound | +631 MiB |
| Buffer-cache arena (resident) | 1024 MiB |
| Steady guest with demand arenas, no STOP, ~5 min | ~2650 MiB |
| Level load running with the copy path active | up to ~5056 MiB peak (pre-fix failure case) |
| Gameplay >600 s, copies bypassed, after the arena fixes | 3837–3897 MiB peak (menu; not gameplay) |
| Real gameplay, manual Continue, copies skipped (`.1415`) | 4 411 MiB in one capture, 3–4 FPS |

Those are different runs on purpose: the boot/renderer numbers explain fixed cost, the steady
number shows the demand-allocated base, and the ceiling number is the failure case described in
[KNOWN_ISSUES](KNOWN_ISSUES.md).

## 3. Three arenas of 512 MiB

The renderer uses **three arenas of 512 MiB**. This is a closed decision based on console
evidence, not a tuning knob: changing the count or the size requires new measurements from the
console that justify it.

On UWP the arenas are **host-cached** (`ArenaMemoryType()` returns `HostCached`) and are **not**
sparse. They are allocated on demand (`BB_ARENA_DEMAND`, the default), so the process does not
pay 1536 MiB up front; set `BB_ARENA_DEMAND=0` only to reproduce the eager behaviour.

Arena memory is part of the app's allocation accounting, so it is already inside the 5 GiB. Do not
add it to the buffer/image figures as if it were extra.

## 4. Where the memory actually goes

A quantised sample during level loading (`BB_VRAM_LOG=1`):

| Counter | Value |
| --- | ---: |
| Process memory | 2654 → 2886 MiB |
| Live allocations | 2557 → 2781 |
| Buffers | 2525 |
| Images | 181 (248 with their views) |
| Arenas reported by the counter | 0 |

With the three 512 MiB arenas inside the allocation total, roughly **989 MiB of guest buffers**
remain on top. The growth is in **buffers**, not images, and `BufferCache` has no LRU or trim
policy: once a buffer is created it lives until the process ends.

This was the hard problem, and two bugs explain most of it:

- `GetArena` mapped address-space pages with `size >> arena_page_bits`. A demand-sized arena below
  one 512 MiB page mapped **zero** pages, so a destroyed arena stayed referenced and the same
  cross-page request merged again — nine times in one measured run, growing 512 → 576 → … → 1088
  MiB. It now publishes both virtual pages and unpublishes every alias before destroying.
- Anchoring the window at the 512 MiB page base turned a 0.1–2.8 MiB access near the page end into a
  320–512 MiB contiguous allocation, which Dozen refuses (all memory types sit in one 1280 MiB heap
  that the ICD overcommits) long before the 5120 MiB app ceiling. Windows are anchored in the
  existing 64 MiB quantum, and `covers()` rebuilds safely on a later distant access.

Idle large staging buffers are also retired after ~4 s under pressure instead of 50 s.

## 5. Allocations that were fixed

A fixed-size scratch buffer was the first OOM:

- `Runtime::CopyColorAndDepth` reserved **128 MiB per copy**, regardless of the copy size.
- Under `UNCHARTED_UWP` it now uses the real size, verified on console:
  `M5 depth scratch #1..#7: 4500 KiB instead of 131072 KiB`, with no `M5 buffer alloc FAILED`.

That removed the scratch OOM but did not remove the copies themselves, which are the current
hang trigger (see [KNOWN_ISSUES](KNOWN_ISSUES.md)).

## 6. Levers already ruled out

| Hypothesis | Test | Result |
| --- | --- | --- |
| Decommitted pages leaking | enable the compiled-out `VirtualFree(MEM_DECOMMIT)` (`BB_GUEST_DECOMMIT=1`) | `released=0`, same ceiling — not a detached-page leak |
| Shader cache growing too early | flush/limit the cache, rebuild on demand | 335→261 and 346→282 compiled pipelines, no measurable reduction in the 2.5 GiB of buffers |
| Device heap limit | compare VMA budgets with the app budget | the app budget is the real limit, not the heap |
| Scratch copy buffer | size copies from the real dimensions | fixed, but the copy path still hangs |

## 7. How to measure

- The overlay is mandatory and always available: current and peak app memory against 5120 MiB,
  FPS, frame time, and known buffer/image/arena consumption where the renderer exposes it.
- Never judge a change without a before/after sample from the **console**; a desktop build cannot
  prove console behaviour.
- Take samples at the same point (after the first real frame, during the same load step) and keep
  the peak, not just the last value. Peaks are what kill the process.
- Prefer A/B runs with a single switch and one variable. Multi-change runs cannot attribute cost.
- `BB_VRAM_LOG=1` adds the allocation counters shown above; it is diagnostic and adds overhead.

## 8. Rules for memory changes

1. Never retain buffers, images, caches, pages or objects that are not needed at that moment.
2. Measure the peak, not the average; an optimisation that raises the peak is not acceptable.
3. Precompiled caches and pipelines load on demand and may not keep an entire library resident.
4. A change that only moves memory from one consumer to another (for example, GPU to app) is not
   an improvement.
5. Every claim needs a console log line. "It fits on PC" is not evidence.
