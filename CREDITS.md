# Credits and acknowledgements

This port is a thin layer on top of work done by other people. Everything below is genuinely used
to build, run or debug it; nothing in this repository includes game code or game assets.

> The porting work and documentation themselves were produced by AI agents; see
> [AI_DISCLOSURE](AI_DISCLOSURE.md). The projects credited here are human work.

## Base projects this port was built from

| Project | Author | What this port uses |
| --- | --- | --- |
| [uncharted_pc](https://github.com/Supermedo/uncharted_pc) | SuperMedo and contributors | The PC compatibility layer: guest runtime, shadPS4-derived renderer, launcher, patch tools (GPL-2.0-or-later). |
| [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) (bbport) | deadinside28 | The original native port architecture, the `bbport_*` layer, overlay, settings and launcher framework this tree still carries (GPL-2.0). |
| **gow3_xbox** | Cosmo | The proven Xbox UWP host: activation, packaging, guest-memory pager, Dozen integration and console scripts, adapted here (GPL-2.0). |
| **SpartanPort** | — | The Xbox compatibility base that the gow3_xbox host derives from. |
| [shadPS4](https://github.com/shadps4-emu/shadPS4) | shadPS4 team | HLE libraries (GNM driver, VideoOut, Videodec2), shader recompiler and Vulkan renderer (GPL-2.0). |
| [shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR) | IFreemz | The DLSS bridge, Videodec2 and upscaler work adapted into `gpu/dlss_bridge` and the renderer. |
| **KytyPS5 for Xbox** | Kyty contributors | Reference design for the cold-page guest pager (`xbox/platform/guest_swap.inc`), adapted rather than vendored. |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa) / Dozen | Mesa and Dozen contributors | The Vulkan 1.2 implementation on D3D12 used on the console, through the Dozen fork maintained for this port. |
| [Uncharted PC port](https://github.com/bc160201983/Uncharted-The-Nathan-Drake-Collection-PC-port) | BilalReloaded | Fixes carried into the PC layer (GPL-2.0): cloth-solver barriers, guest commits on demand, look-at reset, descriptor fixes, `GOW3_COPY64_EOP_READBACK` copy path. |
| [community builds](https://github.com/drakolordx7/uncharted_pc) | drakolordx7, Mevlock, Alex, iannitram | Fixes merged into the upstream PC port (texture handling, frame-generation leak, large compute workgroups). |
| [dev builds](https://github.com/Mevlock/uncharted_pc-dev) | Mevlock | Additional fixes and diagnostics from the upstream development builds. |

Thanks to **Mindro** for the shadPS4 Uncharted build that showed which problems to look for, and to
the upstream PC-port contributors named in its documentation (Jhojan Jiménez, FerManjarres,
dbcalo, mato215, Umar-Qayyum-dev) whose fixes are part of the layer this port runs on.

The patch catalog under `patches/` is the upstream patch-engine fixture: it describes patches for
the game that layer was first written for, and is kept only because `scripts/patches.py` and
`tests/test_patches.py` use it as their reference document. It is not used by the Uncharted UWP
build.

## Vendored libraries

| Library | Purpose | License |
| --- | --- | --- |
| [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) | AMD FidelityFX Super Resolution on Vulkan (FSR 3/4 assets from [Q2RTX](https://github.com/FireBurn/Q2RTX)) | MIT-style; see upstream |
| [Dear ImGui](https://github.com/ocornut/imgui) | In-app overlay and launcher widgets | MIT |
| [sirit](https://github.com/shadps4-emu/sirit) | SPIR-V assembly/emission | BSD-2-Clause |
| [magic_enum](https://github.com/Neargye/magic_enum) | Enum reflection | MIT |
| [miniz](https://github.com/richgel999/miniz) | Deflate/zlib | MIT |
| [xbyak](https://github.com/herumi/xbyak) | x86 JIT assembler | BSD-2-Clause |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Vulkan API headers | Apache-2.0 |
| [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9) | ATRAC9 audio decoding | MIT |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | GPU allocation | MIT |
| [SDL3](https://github.com/libsdl-org/SDL) | Desktop window and input path | zlib |
| [FFmpeg](https://ffmpeg.org) | Video decoding (static, configured LGPL build) | LGPL-2.1+ |
| [Cinzel](https://github.com/NDISCOVER/Cinzel) | Launcher font | SIL Open Font License 1.1 |

Optional SDKs referenced by the PC layer but not shipped here: [NVIDIA DLSS](https://github.com/NVIDIA/DLSS),
[Intel XeSS](https://github.com/intel/xess) and the AMD FidelityFX SDK. Each keeps its own license.

Build tools used: [MSYS2](https://www.msys2.org), [LLVM/Clang](https://llvm.org),
[CMake](https://cmake.org), [Ninja](https://ninja-build.org), [Python](https://www.python.org),
[PyInstaller](https://pyinstaller.org) and [Pillow](https://python-pillow.org).

Dependencies are fetched at pinned versions and checked against their SHA-256 by
`xbox/scripts/fetch-deps.py`; each one keeps its own license file in `xbox/.local/deps` after
fetching. Vendored source trees under `gpu/third_party/` and `third_party/` keep their upstream
license files.

## Artwork

All artwork in this repository is generated from code, not copied:
`packaging/windows/make_uncharted_art.py` draws the launcher scenes and emblem, and
`xbox/assets/make_app_icon.py` draws the UWP package artwork (procedural parchment, a generic
explorer silhouette and a wordmark). No official key art, cover, logo, screenshot or character
likeness is included or traced; protected covers and game screenshots were deliberately left out.

## Legal notice

*Uncharted*, *Uncharted 2: Among Thieves*, *Uncharted 3: Drake's Deception* and *The Nathan Drake
Collection* are trademarks of Sony Interactive Entertainment. *Uncharted* is a Naughty Dog
property. NVIDIA, GeForce, RTX and DLSS are trademarks of NVIDIA Corporation. AMD, Radeon and
FidelityFX are trademarks of Advanced Micro Devices. Microsoft, Xbox and DirectX are trademarks of
Microsoft Corporation.

This project is not affiliated with, endorsed by or supported by any of them. It ships no game
data and is intended for use with legally obtained, personally decrypted game files only.
