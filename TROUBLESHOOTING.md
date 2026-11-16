# Troubleshooting

Console evidence is the only evidence that counts. Before changing code, get the log and identify
the last step that ran.

## 1. Where the logs are

| File | Contents |
| --- | --- |
| `LocalState/loader.log` | Loader, runtime and renderer steps, including the build stamp. |
| `LocalState/errors.log` | Startup and service failures. |
| `LocalState/raw.log` | Raw markers written by `bb_log_raw` (`M5 dump:`, `M5 pso:`, ...). |

`raw.log` is **appended across runs**. Delete it before a diagnostic run, otherwise old lines
look like fresh evidence.

`stderr` is not a valid channel inside an AppContainer. If something only writes to stderr, its
output is lost; log through stdout or a file instead.

Download logs through Device Portal:

```powershell
./xbox/scripts/xbox-download.ps1 -Url https://<console>:11443/ -Insecure `
    -App <PackageFullName> -RemotePath LocalState/loader.log -MaxMiB 8
```

Logs can be downloaded while the app is running. Files are flushed when the worker closes; the
startup watchdog closes them too if initialisation dies.

## 2. Read the log in this order

1. Build stamp — confirm the console is running the package you think it is.
2. Last successful step, then the first failure line (not the last line).
3. `HRESULT` on Windows failures (`0x887A0006` is `DXGI_ERROR_DEVICE_HUNG`).
4. Thread id and fault address, if present.
5. Peak app memory against 5120 MiB.

## 3. Common failures

### The app does not install

- Installation takes the exact **PackageFullName**
  (`UnchartedNDCUWP_<version>_x64__<publisher-hash>`), not the family name.
- The package must be signed by a certificate the console trusts. Use a local test key with
  subject `CN=uncharted-uwp` (see [BUILD](BUILD.md)); the repository ships none.
- Install the `Microsoft.VCLibs.x64.14.00.appx` dependency from the same package folder.
- A new package installs over the old one and keeps `LocalState`.

### The app does not launch

- Run `xbox-stop.ps1` first; the console keeps stale process records and refuses or stalls.
- `HTTP 400` from the launch endpoint is **not** a failure. Check the app log for the new build
  stamp instead.
- The first launch initialises `LocalState`; configuration uploads require that folder to exist.
- Uploads refuse to overwrite an existing file unless `-Overwrite` is passed.

### The console data is wrong or missing

- The app reads its dump from the configured path (`E:\dumps\<folder>` for the console run).
  A missing or half-copied dump shows up as a prepare failure, not as a graphics failure.
- `Prepare` runs once and stores a fingerprint in `LocalState/Prepared`. Change the dump or the
  patch file and it prepares again; this is expected.
- `run.txt` line 1 selects the run mode and line 2 the dump folder. Without `run.txt` the app
  behaves like a normal player install.

### Scripted input does nothing

- `LocalState/pad.txt` is a **held state**, not a pulse. A single write that sets and clears a
  button within one frame can be missed.
- The runtime re-reads the file when its **size** changes, because modification times have
  one-second granularity. Write a different length, or append a newline.
- Release scripted input when the run ends, otherwise the game keeps holding the button.

### The run stops at the menu

- Check that the guest reached the real menu (27 threads) and that input is being delivered.
- The menu, a real frame and a completed level load are progress; none of them is M5.

### `DXGI_ERROR_DEVICE_HUNG` / device lost

- Look for `M5 bridge lost at wait: native_removed=0x887a0006` next to a failing
  `vkWaitForFences` returning `VK_ERROR_DEVICE_LOST`.
- `VK_EXT_device_fault` does **not** exist in the console ICD, so there is no fault report from
  the driver. Use the GPU history ring (`BB_GPU_HISTORY=1`) to see the last submitted work.
- `BB_SKIP_COPY=1` removes the hang for diagnosis but is **not** a fix: it discards the colour and
  depth copies the game needs.

### Memory assert

- `M5 ASSERT buffer.cpp:318` with `used=... peak=... limit=5120` means the app hit the absolute
  ceiling. `committed` and `released` counters show whether the memory was still live.
- Read [MEMORY](MEMORY.md) before proposing anything; several obvious explanations were measured
  and rejected.

### Determinism mismatch

- The official anchor is the hash of the pipeline-state sequence, always quoted with the package
  number it came from. Never compare hashes across packages.
- Guest multithreading makes raw hashes unstable: the `entry` mode has 237 records in 3 of 3 runs
  but assigns them differently between job-system threads. Use the multiset mode of the
  three-run comparison (`run-three.ps1 -Multiset`) rather than a raw hash.

## 4. Run modes and diagnostic switches

`run.txt` line 1 accepts: `entry`, `entry-unresolved`, `entry-draw`, `entry-realgnm`,
`entry-realgnm-draw`, `entry-staticpso`, `entry-notess`, `entry-notess-full`, `entry-flipverify`,
`entry-dump-cs`, `entry-skip-loop-cs`, `entry-skip-copy`, `entry-precompile`, `entry-memlog`,
`renderer-boot`.

Useful switches (diagnostic only, never leave them in a normal profile):

| Switch | Effect |
| --- | --- |
| `BB_SKIP_COPY=1` | Discards colour/depth copies. Default on UWP; `=0` restores them. |
| `BB_SKIP_COPY=2` / `=3` | Skips only depth↔colour copies / only same-format copies. |
| `BB_DUMP_SHADERS=1` | Dumps the active SPIR-V set (`M5 dump:` marker in `raw.log`). |
| `BB_SKIP_COMPUTE=<hash>` | Discards dispatches of one compute shader. |
| `BB_GPU_HISTORY=1` | Records the last submitted work items with scheduler ticks. |
| `BB_FULL_BARRIER=2` | Inserts full barriers everywhere (barrier experiment). |
| `BB_VRAM_LOG=1` | Periodic allocation/buffer/image counters. |
| `BB_ARENA_DEMAND=0` | Eager arena allocation instead of demand. |
| `BB_GUEST_DECOMMIT=1` | Enables the guest `MEM_DECOMMIT` path. |

## 5. Checklist before reporting a bug

- Package version and build stamp.
- `run.txt` mode and every `BB_*`/`SPARTAN_*` switch that was set.
- Console log excerpt around the failure (not the whole file).
- Peak app memory and the exact assert or HRESULT.
- Whether the same step reproduces twice.
