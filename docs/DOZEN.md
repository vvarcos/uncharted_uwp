# Mesa / Dozen source dependency

The Mesa source is included as a Git submodule at `third_party/mesa`:

- Fork: https://github.com/Cosmo/mesa_ai_slop_version
- Branch: `dozen-update`
- Pinned source commit: `82804960ac7919b24dbf27d1cec2b4d637573731`
- Upstream: https://gitlab.freedesktop.org/mesa/mesa

The fork is identified by the Xbox port's upstream credits. Its sources include the Dozen driver under `src/microsoft/vulkan`, shared Mesa code, build definitions, and the fork's changes. No additional local Mesa patch set was found in this workspace. The submodule is unmodified; this project does not claim authorship of its changes.

## Obtain the files

For a new clone:

```powershell
git clone --recurse-submodules https://github.com/vvarcos/uncharted_uwp.git
```

For an existing clone, run from the repository root:

```powershell
git submodule update --init --depth 1 third_party/mesa
git -C third_party/mesa rev-parse HEAD
```

The last command should print the pinned commit above. Use `submodule update`, not `submodule update --remote`, to stay on the reviewed source revision. A normal GitHub ZIP does not contain submodule contents; use Git or download the source archive for the pinned commit separately.

## Source revision versus tested DLL

The driver used in the existing console experiments was extracted from an upstream gow3 Xbox app package, rather than built from a Mesa source tree in this workspace. Its SHA-256 is:

```text
915642B91309796291BF38E228C7898B6389BDD92496852760AB5DE48B43D2E3
```

The exact source commit and toolchain that produced that DLL have **not** been established. The source pin above records the available fork revision; it is not a claim that rebuilding it reproduces the tested binary or its hash. No Microsoft console DLLs, signing certificates, game assets or entire extracted APPX are redistributed here.

## Building and packaging

The app's `xbox/scripts/build.ps1 -Graphics` **does not compile Mesa**. It reads `dozen_dll` and `dxil_dll` from ignored `xbox/.local/settings.json`, then passes those prebuilt inputs to the app build. Compiling the driver is a separate Mesa/Meson task.

Start with the fork's build definitions and the Meson documentation in the included source. A verified UWP cross-file, complete toolchain recipe and source-to-tested-DLL mapping remain to be documented. Do not assume that a desktop Dozen DLL is usable in AppContainer merely because it exports Vulkan entry points.

After producing a compatible `vulkan_dzn.dll`, set `dozen_dll` to its path and `dxil_dll` to a separately licensed compatible DXC runtime, then build/package the app as described in [BUILD](../BUILD.md). Do not commit private settings or generated build directories.

## Licensing

Mesa is not covered solely by this port's GPL license. Preserve its own licensing and copyright notices:

- `third_party/mesa/docs/license.rst`
- `third_party/mesa/licenses/`
- Per-file SPDX identifiers and copyright notices.

Most Mesa code uses MIT, but individual components have other licenses. The source checkout retains the original notices. Redistributing a built driver requires checking the components actually linked into that binary; the generic license template shipped in an older package is not a substitute for that audit.
