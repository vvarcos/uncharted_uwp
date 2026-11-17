# Building and deploying

This document describes how to build the UWP app, the Dozen driver and the package, and how to
put the result on a console. All generated files stay under `out/` and `xbox/.local/`; neither is
part of this repository.

## 1. Host requirements

- Windows 10 1903+ or Windows 11, x64.
- Visual Studio 2022 with the C++ workload, clang-cl and the Windows 10/11 SDK
  (`WindowsSdkVerBinPath` must resolve).
- CMake 3.20+, Ninja, Git, Python 3.10+.
- MSYS2 (CLANG64 environment), installed at `C:\msys64` unless `-Msys2` is passed to the scripts.
- PowerShell 7.2+.
- The Mesa/Dozen source submodule at `third_party/mesa` (initialize it with
  `git submodule update --init --depth 1 third_party/mesa`), plus compatible prebuilt UWP Dozen
  and DXIL DLLs. See [source pin and provenance limitations](docs/DOZEN.md).

## 2. Configuration

`xbox/scripts/setup.ps1` writes the private configuration. It refuses to overwrite an existing
setup.

```powershell
./xbox/scripts/setup.ps1 `
    -PcDump    D:\dumps\uncharted `
    -XboxDump  E:\dumps\uncharted `
    [-MesaRoot C:\Projects\Mesa] `
    [-Python   python]
```

It creates:

- `xbox/.local/settings.json` — dump paths, Python and Dozen checkout (schema:
  `xbox/settings.example.json`).
- `xbox/.local/devices.json` — console address (schema: `xbox/devices.example.json`).
- `xbox/.local/deps/` — pinned dependencies fetched and verified by `fetch-deps.py`.

Console credentials are **only** read from the `XBOX_USERNAME` and `XBOX_PASSWORD` environment
variables. No credential is ever written to the repository, and none is required to build.

## 3. Dependencies

`xbox/scripts/fetch-deps.py` downloads every third-party library and tool at a pinned version from
its upstream source, checks its SHA-256 and unpacks it into `xbox/.local/deps` together with its
license. `xbox/scripts/build-deps.ps1` then builds the two dependencies that need compilation:

- **glslang** — the shader front end used by the offline/precompile path.
- **FFmpeg 7.1.1**, static and LGPL, limited to the codecs and demuxers the game uses.

Both build with clang-cl through MSYS2. Nothing is fetched from the network at runtime by the
shipped app.

## 4. Dozen (the Vulkan driver)

The console has no native Vulkan driver. This port uses Mesa **Dozen** over D3D12. The fork's
sources are included as the pinned `third_party/mesa` submodule, including its own license notices.
See [DOZEN](docs/DOZEN.md) for the fork URL, source commit and the tested DLL's checksum.

Compile Mesa separately with a compatible UWP toolchain, or supply a compatible prebuilt driver.
The exact source revision/toolchain of the previously tested DLL and a verified driver-build recipe
are not yet established; the source pin alone must not be presented as a reproducible binary build.
Set `dozen_dll` and `dxil_dll` in ignored `xbox/.local/settings.json`, then run:

```powershell
./xbox/scripts/build.ps1 -Graphics
```

This command builds the UWP app with the supplied binaries; it does **not** compile Mesa. The
Mesa sources retain their upstream licenses, independently of this port's license. Driver and
DXIL binaries are not committed by this source-dependency update.

Dozen's limits are real constraints, not preferences:

- no buffer device address, no sparse residency, no push descriptors, no robustness2,
  no descriptor indexing;
- no tessellation shaders (rect/quad list draws are emulated);
- non-null `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` results do **not** prove that a function
  is implemented: Dozen returns an internal trampoline that forwards to a null entry.

## 5. Build the UWP app

```powershell
./xbox/scripts/build.ps1 -HeadlessDevice
```

This produces the executable, `bbprobe.dll` and `bbgpu_dll.dll` in `xbox/.local/build/`. Before
packaging, copy `bbgpu_dll.dll` into `xbox/.local/build/layout/`:

```powershell
Copy-Item xbox\.local\build\bbgpu_dll.dll xbox\.local\build\layout\
```

Verification helpers:

```powershell
./xbox/scripts/check-imports.ps1          # lists unresolved imports
./xbox/scripts/verify-m2.ps1              # guest memory contract
./xbox/scripts/verify-m4.ps1              # renderer/present contract
./xbox/scripts/verify-m5-contract.ps1     # M5 contracts
```

## 6. Sign and package

The repository ships **no signing key**. Generate a self-signed test key whose subject matches the
manifest publisher, `CN=uncharted-uwp`, and export it to `xbox/.local/signing.pfx` (the path
`package.ps1` reads):

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=uncharted-uwp' `
    -KeyUsage DigitalSignature -FriendlyName 'uncharted-uwp test' `
    -CertStoreLocation 'Cert:\CurrentUser\My'
Export-PfxCertificate -Cert $cert -FilePath xbox\.local\signing.pfx -Password (Read-Host -AsSecureString)
```

Trust the certificate for the current user (`Import-Certificate` into `Cert:\CurrentUser\Root`), or
edit `xbox/AppxManifest.xml` to match your own publisher before building. The key is a local test
key; it vouches for nobody and must never be committed.

Packages signed with a different publisher than an already installed build do not upgrade it: the
console installs them as a separate app family. Uninstall the old development package once when
you change the publisher.

```powershell
./xbox/scripts/package.ps1
```

The package is written under `xbox/.local/packages/<timestamp>/` with its layout, manifest and
`Microsoft.VCLibs.x64.14.00.appx` dependency. Keep both files together when installing.

## 7. Deploy to the console

Developer Mode exposes Device Portal on `https://<console>:11443/`. The scripts take the address
explicitly; nothing is hardcoded.

```powershell
./xbox/scripts/xbox-install.ps1 -Url https://192.168.1.100:11443/ -Insecure -Package <appx>
./xbox/scripts/xbox-launch.ps1  -Url https://192.168.1.100:11443/ -Insecure -App <PackageFullName>
./xbox/scripts/xbox-status.ps1  -Url https://192.168.1.100:11443/ -Insecure -App <PackageFullName>
./xbox/scripts/xbox-stop.ps1    -Url https://192.168.1.100:11443/ -App <PackageFullName>
```

- `-Insecure` accepts the console's self-signed TLS certificate.
- Installation, launch and stop take the exact **PackageFullName**
  (`UnchartedNDCUWP_<version>_x64__<publisher-hash>`), not the family name.
- Stop the previous run before launching a new one; the console keeps stale process records.
- A `HTTP 400` from the launch endpoint does **not** mean the launch failed; confirm with the build
  stamp in the app log.

Game files are copied to a USB drive as `E:\dumps\<folder>`. The app prepares them once
(`Prepare`), then optionally precompiles shaders, then starts the game.

## 8. PC build (desktop path)

The desktop compatibility layer still builds on Windows and Linux:

```bash
# MSYS2 CLANG64 on Windows
bash build.sh              # -> out/bb-probe.exe
bash packaging/windows/package.sh   # -> dist/uncharted-windows + zip
```

This path is useful for reproducing renderer behaviour without a console, but it does **not**
validate console behaviour: only console logs count as evidence for this port.

## 9. Repository hygiene

- Never commit `out/`, `xbox/.local/`, packages, certificates, logs, captures, game dumps or
  prepared game code; the `.gitignore` files cover them.
- Do not commit proprietary binaries. Vendored source trees are kept in-tree without their `.git`
  metadata.
- Keep generated evidence outside the repository; link to it in issues instead.
