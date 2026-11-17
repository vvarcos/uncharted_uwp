# Uncharted: The Nathan Drake Collection — Xbox (UWP) port

An experimental port of the PC compatibility layer for *Uncharted: The Nathan Drake Collection*
to the **Xbox Series S|X** in Developer Mode. The original PS4 game code runs directly on the
console; graphics calls are translated to Vulkan and handed to **Dozen**, a Mesa driver that
implements Vulkan 1.2 on top of D3D12 (feature level 11_0, shader model 6.4).

This repository is a snapshot of the work in progress. It contains **no game code and no game
assets**. You need your own decrypted dump of the collection.

> **Status: M5 gate met on console; the copy path is still bypassed.**
> M1 (UWP loader), M2 (sparse/demand guest memory, original entry), M3 (memory and runtime
> services), M4 (connected renderer, movies and 3D menu) and the M5 gate (24+ pipelines, three
> arenas, 600 s of continuous gameplay, peak under 5000 MiB) are measured on hardware. What is
> **not** solved is rendering correctness: the colour/depth copy path still triggers
> `DEVICE_HUNG`, so `BB_SKIP_COPY=1` stays on and the image is approximate. Treat that as the
> open item, not the milestones.

> **AI-generated work.** Every change in this repository — code, scripts, documentation, audits and
> Git history — was produced by AI agents under human direction. Read
> [AI_DISCLOSURE](AI_DISCLOSURE.md) before relying on or forking this port.

## What currently works

- The UWP package activates, registers and installs in Developer Mode.
- The guest image maps, relocates and executes the game's own entry point.
- libc/Fios/module initialisation completes and the runtime reaches the real game menu
  (27 guest threads, Friend Stats dialog, level loading with scripted input).
- The graphics stack is connected: Dozen loads, the renderer creates pipelines, and frames are
  presented through the CoreWindow bridge.
- With the colour/depth copy path bypassed (diagnostic mode only) the first scene renders at
  720p and the process survives several minutes of presentation.
- Three independent runs held gameplay for over 600 s each at a peak of 3837–3897 MiB against the
  5120 MiB ceiling, with 31 graphics and 17 compute pipelines compiled on demand in one of them.
- The console HUD draws current/peak/limit RAM and FPS/frame time over the game; the peak is
  sampled on every present, so it is authoritative rather than a 2 Hz estimate.
- Button prompts stay the dump's PlayStation glyphs instead of being replaced with PC keys.

## What does not work yet

- The level load still ends in `DXGI_ERROR_DEVICE_HUNG (0x887A0006)` when the colour/depth copy
  path is enabled. See [KNOWN_ISSUES](KNOWN_ISSUES.md).
- With that path bypassed, a run reaches the **5120 MiB app-memory ceiling** instead: the buffer
  cache grows without an eviction policy until an allocation assert fires.
- Upscalers (FSR, DLSS, XeSS), frame generation and the temporal upscaler paths are disabled on
  the console build.
- No performance number in this repository is a validated frame-rate claim.

## Non-negotiable constraints

1. **5120 MiB is an absolute ceiling** for the app. It is not a target and cannot be raised.
   Application memory, game memory and GPU memory are shared: every MiB the GPU reserves is one
   MiB the game cannot use. See [MEMORY](MEMORY.md).
2. **Three 512 MiB arenas stay.** They are a measured console decision and are not re-opened
   without new console evidence.
3. **Vulkan 1.2 through Dozen only.** There is no native Vulkan driver on the console, and Dozen
   does not expose buffer device address, sparse residency, push descriptors, robustness2 or
   tessellation shaders.
4. **No proprietary content.** No game dump, no package signing key, no captures, no console
   addresses and no credentials live in this repository.

## Requirements

- An Xbox Series S or Series X with Developer Mode enabled.
- Your own decrypted dump of the collection (the folder containing `eboot.bin`,
  `big2-ps4_Shipping.elf`, `big3-ps4_Shipping.elf`, `sce_sys`, `u1data`, `u2data`, `u3data`).
- A Windows 10/11 build machine with Visual Studio 2022 C++ tools (clang-cl), a Windows SDK,
  CMake 3.20+, Ninja, Python 3 and MSYS2.
- A copy of the Mesa/Dozen fork used by this port; it is not vendored here.

## Quick start

```powershell
# 1. Fetch and build the pinned dependencies (writes only to .local/).
./xbox/scripts/setup.ps1 -PcDump D:\dumps\uncharted -XboxDump E:\dumps\uncharted

# 2. Build the UWP app and Dozen.
./xbox/scripts/build.ps1 -HeadlessDevice

# 3. Package and deploy to the console through Device Portal.
./xbox/scripts/package.ps1
./xbox/scripts/xbox-install.ps1 -Url https://<console>:11443/ -Insecure -Package <appx>
./xbox/scripts/xbox-launch.ps1  -Url https://<console>:11443/ -Insecure -App <PackageFullName>
```

Full details, including the signing key you must generate yourself, are in [BUILD](BUILD.md).

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | Guest runtime: kernel, memory, threads, files, pad, audio, video out, savedata, trophies. |
| `gpu/shadps4/` | shadPS4-derived Vulkan renderer, HLE libraries (GNM driver, VideoOut, Videodec2) and shader recompiler. |
| `gpu/shim/` | UWP adapters that bridge guest graphics services to the host renderer. |
| `gpu/third_party/` | Vendored renderer dependencies (FSR-Vulkan, ImGui, sirit, magic_enum, miniz, xbyak, ...). |
| `xbox/platform/` | UWP host: activation, guest memory, disk pager, audio, input, game preparation. |
| `xbox/graphics/` | CoreWindow presentation bridge, swapchain, overlay, texture probes. |
| `xbox/scripts/` | Setup, dependency fetch, build, packaging, console deploy and verification scripts. |
| `patches/` | Game patch data consumed by the loader. |
| `launcher/` | Desktop launcher sources (PC path; not shipped to the console). |
| `tests/` | Native and Python tests for the runtime, memory contract and launcher. |
| `docs/` | Upstream PC-port documentation kept for reference. |

## Documentation

- [BUILD](BUILD.md) — toolchain, dependencies, packaging, signing, deployment.
- [ARCHITECTURE](ARCHITECTURE.md) — how the loader, renderer, graphics bridge and host fit together.
- [MEMORY](MEMORY.md) — the 5120 MiB ceiling, the three arenas and how memory is measured.
- [TROUBLESHOOTING](TROUBLESHOOTING.md) — logs, run modes, Device Portal problems.
- [KNOWN_ISSUES](KNOWN_ISSUES.md) — current blockers and their evidence.
- [HISTORY](HISTORY.md) — milestone timeline.
- [CREDITS](CREDITS.md) — upstream projects, libraries and licenses.
- [AI_DISCLOSURE](AI_DISCLOSURE.md) — how this repository was produced, and what the human did.

## Legal

No game files are included or downloaded by this project. *Uncharted* and *The Nathan Drake
Collection* are trademarks of Sony Interactive Entertainment; *Uncharted* is a Naughty Dog
property. This project is not affiliated with, endorsed by or supported by Sony Interactive
Entertainment, Naughty Dog, Microsoft or NVIDIA.

The port is licensed under the **GNU GPL v2 or later**; see [LICENSE](LICENSE). Vendored
components keep their own licenses, listed in [CREDITS](CREDITS.md).
