# Architecture

The port keeps the existing PC compatibility layer and adds a UWP host, a console memory model and
a presentation bridge. Everything that is console-specific lives under `xbox/` and behind
`#ifdef`s; the desktop build must keep working.

```
┌──────────────────────────────────────────────────────────────────────────┐
│ UWP app process (uncharted_uwp.exe)                                      │
│                                                                          │
│  Xbox platform host          Guest runtime (src/)        Launcher UI      │
│  xbox/platform/              runtime_memory, kernel,     (optional ImGui  │
│  activation, logs,           threads, file, pad, audio,   overlay)        │
│  watchdog, input, audio      display, savedata, trophy                   │
│           │                         │                                    │
│           │            ┌────────────┴─────────────┐                      │
│  xbox/platform/        │  Graphics shim (gpu/shim) │  guest GNM/VideoOut  │
│  guest_memory.inc  ────┤  HLE libraries            ├── Videodec2 calls    │
│  guest_swap.inc        │  gpu/shadps4/core/...     │                      │
│                        └────────────┬─────────────┘                      │
│                                     │ Vulkan 1.2 API                      │
│  xbox/graphics/          ┌──────────┴──────────┐                          │
│  present_bridge.cpp ─────┤  Dozen (vulkan_dzn) │──── D3D12 / DXIL          │
│  swapchain, CoreWindow   │  Mesa Vulkan → D3D12│                          │
│                          └──────────┬──────────┘                          │
│                                     │                                    │
│                          D3D12 device + DXGI swapchain                    │
└──────────────────────────────────────────────────────────────────────────┘
```

## 1. UWP host

`xbox/platform/app_host.cpp` and `main.cpp` own activation, the private configuration folder
(`LocalState`), log files and the startup watchdog. The host is deliberately thin: it brings up
the runtime, loads the guest image and forwards lifecycle events (visibility, suspend, resume,
back request).

Two AppContainer rules shape everything:

- **stderr is not a valid channel.** Diagnostics go to stdout or to files under `LocalState`.
- `SetThreadDescription` is not part of the allowed contract; thread names are kept in a small
  registry keyed by handle/hash.

`xbox/platform/input.cpp` uses WinRT gamepad APIs and feeds both the guest pad and the host save
dialog. Scripted input is read from `LocalState/pad.txt`; the file is a **held state**, not a
pulse, and the runtime re-reads it based on file size because modification times have
one-second granularity.

`xbox/platform/audio.cpp` implements the audio sink on XAudio2 with the runtime's steady buffer
pacing; `SPARTAN_AUDIO=none` keeps a silent path for diagnostics.

`xbox/platform/prepare_game.cpp` prepares the user's dump once (linked game code, content profile
and game patches) into `LocalState/Prepared`, recording a fingerprint so a changed dump or patch
set triggers a re-prepare. It contains no game data.

## 2. Loader and guest runtime

`src/probe.c` maps the linked guest image, applies relocations and redirects TCB loads to the
Windows TLS slot. On the console the early whole-range reservation is deferred so code loading can
be tested independently; the guest allocator then reserves on demand.

`src/runtime_*.c` provide the guest kernel services: memory, threads and fibres, mutexes,
semaphores, reader/writer locks, time and RTC, files, pads, audio, display, savedata and trophies.
Each service is reported explicitly when unimplemented; no host implementation is silently
substituted for an unported guest service.

## 3. Memory model

The guest sees 16 KiB pages; the host protects memory at 4 KiB because the GPU tracks mapping
changes at that granularity.

- Guest windows are reserved on demand and committed when touched.
- Direct-page contents survive unmap/remap; live aliases are promoted to small shared sections so
  alias coherence stays real without one giant section.
- `xbox/platform/guest_memory.inc` holds the commit/protection/alias contract;
  `xbox/platform/guest_swap.inc` implements a cold-page pager for large GPU mappings.
- The pager uses a single sparse temporary file under `LocalState`, a bounded shared frame cache
  (64 MiB) and cached `ReadFile`/`WriteFile` transfers at 64-bit offsets. Page-in fills a host
  frame before publishing guest views, so a concurrent reader never sees half-restored bytes.
- Guest code pages, stacks, CPU heaps and small unmapped regions stay pinned; paging is not an
  unlimited-memory guarantee.

See [MEMORY](MEMORY.md) for the budget and the measurements.

## 4. Graphics stack

`gpu/shadps4/` is the shadPS4-derived renderer: HLE implementations for the GNM driver, VideoOut
and Videodec2, a shader recompiler to SPIR-V, and a Vulkan renderer with buffer/texture caches.

`gpu/shim/` adapts those services to this port: guest memory views, the UWP kernel bridge, the
VideoOut contract and the Videodec2 bridge.

`xbox/graphics/present_bridge.cpp`, `swapchain.cpp` and `window.cpp` own the last step: copy the
rendered image into a D3D12 swapchain and present it through CoreWindow. A blue-frame smoke test
is used to prove this path independently of gameplay.

Console-specific renderer decisions:

- **No buffer device address.** Shader constant memory uses a logical page table plus an array of
  ordinary buffer descriptors. Push bindings allocate, update and bind normal descriptor sets.
- **No sparse residency.** Ordinary buffers replace sparse arenas.
- **No tessellation.** Rect lists expand through a geometry shader; quad lists expand to host
  indices (`[0,1,2, 0,2,3]`). Other primitive-restart cases use Dozen's index-format variants.
- **Integer textures.** The Dozen shader model is 6.4, so integer textures are read with
  `OpImageFetch` using a texel coordinate instead of a sampled fetch.
- **Absent resources.** Missing guest buffers/images select shader permutations returning zero,
  backed by real non-null resources, so no descriptor is ever unbound.
- **State baking.** Effective pipeline state is part of the pipeline key and supplied as static
  Vulkan state, because Dozen materialises dynamic depth/stencil, rasterisation and restart state
  as separate D3D12 PSO variants at draw time.

## 5. Compilation and caching

Shader and pipeline compilation is the largest source of first-use stutter, so the console build:

- limits runtime compilation to two workers, one snapshot worker, 16 queued jobs and two retained
  IR pools;
- warms pipelines before the game starts, on all but two hardware threads (at most six);
- persists the driver pipeline cache (`driver.vkcache`) so later starts skip Dozen's SPIR-V to
  DXIL step;
- keeps a separate precompile mode (`entry-precompile`) that waits for the warm-up, saves the
  cache and exits before the guest starts.

Caches are loaded on demand; the whole library is never kept in RAM.

## 6. Presentation and frame pacing

The console profile defaults to 720p, a 60 Hz engine rate, deferred/previous-frame readbacks, the
two-stage draw pipeline, threaded Vulkan recording and two frames queued. Readbacks are required:
the game reads back GPU-written linear images while managing its textures, and skipping them
corrupts asset-to-descriptor pairing.

The in-app overlay (FPS, frame time, current and peak app RAM against 5120 MiB, and known GPU
allocations) is the primary measurement tool. It is intentionally small; profiling switches are
opt-in and off in normal runs.
