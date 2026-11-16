# Known issues

Ordered by how much they block milestone M5. Everything here is reproduced on console; desktop
results are not listed as issues.

## 1. M5 is not closed — no validated gameplay

**Status:** open, blocking the milestone.

The guest boots, reaches the real menu, opens Friend Stats and starts loading the level with
scripted input. With the colour/depth copy path bypassed, the first scene renders and presents for
several minutes. None of that is a validated gameplay run: M5 closes only with 24 pipelines, the
three arenas, 600 seconds of gameplay and a peak below 5000 MiB.

## 2. `DXGI_ERROR_DEVICE_HUNG` on the colour/depth copy path

**Status:** blocker #1. Trigger isolated, root cause not fixed.

At level load, the seventh colour/depth copy leads to `DXGI_ERROR_DEVICE_HUNG (0x887A0006)`.
The graph shows `M5 bridge lost at wait: native_removed=0x887a0006` exactly when
`vkWaitForFences` returns `VK_ERROR_DEVICE_LOST`. The console ICD does not expose
`VK_EXT_device_fault`, so the driver cannot say why.

What is proven:

- **The copy path is the trigger.** With `BB_SKIP_COPY=1` the hang disappears and the game shows
  the real scene at 720p for about five minutes (309 s, 16,767 presentations, 104 vs/ps pairs).
  That mode is diagnostic only: the copies are required.
- **It is not one format.** `BB_SKIP_COPY=2` (depth↔colour copies only, 12 skipped) and
  `BB_SKIP_COPY=3` (same-format copies only, 5 skipped) both still hang. Only removing all copies
  avoids it, so the trigger is the copy path itself — the transfer plus the layout transitions
  common to both classes.
- **The scratch buffer is not involved.** Mode 2 allocates no depth scratch at all
  (`M5 depth scratch` = 0) and still hangs.

Hypotheses already refuted on console — do not re-test them:

- scratch-buffer OOM;
- a `vkQueueSubmit` race;
- a missing barrier (`BB_FULL_BARRIER=2` changed nothing);
- wrong semaphore wait (`eAllCommands`);
- the 3 × 3600-thread dispatches;
- the only looping compute shader (`19367542`, bounded to 18 iterations) and a second compute
  (`6d71bb`): skipping either one leaves the hang unchanged.

Next steps: separate the two copies (colour→colour and depth `0x82`→colour `0x64`, both
1280×720) and replace the copy-through-scratch with a blit or render pass using a persistent
buffer.

## 3. Memory ceiling once the copies are bypassed

**Status:** blocker #2. Measured, not fixed.

With the copies skipped there are no device-hung failures, but the run ends at the absolute app
ceiling: `M5 ASSERT buffer.cpp:318` with `used=5056 peak=5056 limit=5120 MiB` and
`committed=1863 released=0`. A later run hit 5078 MiB.

The growth is in **buffers**, not images: 2525 buffers, 181 images, roughly 989 MiB of guest
buffers beyond the three arenas, and `BufferCache` has no LRU/trim. Decommitting freed nothing
(`released=0`), so the answer must be fewer or smaller allocations, or safe eviction. Details and
the rejected hypotheses are in [MEMORY](MEMORY.md).

## 4. Dozen does not implement everything it advertises

**Status:** permanent constraint of the platform, worked around case by case.

Dozen declares Vulkan 1.2 (device 1.2.363) on D3D12 feature level 11_0 and shader model 6.4. It
has geometry shaders, dynamic rendering, synchronization2 and extended dynamic state. It does
**not** have:

- buffer device address;
- sparse residency;
- push descriptors;
- robustness2;
- descriptor indexing;
- tessellation shaders.

Two practical traps:

- `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr` returning non-null does **not** mean the function
  exists. Dozen returns an internal trampoline that forwards to a null table entry, so guard at
  the point of use, not by feature bit.
- `VK_ERROR_OUT_OF_HOST_MEMORY` from this ICD is generic — a rejected pipeline reports it, so it
  is not a diagnostic for real memory pressure.

## 5. No driver fault report on console

`VK_EXT_device_fault` is absent, so a device lost has no fault address, no vendor message and no
GPU crash dump. Diagnosis relies on the GPU history ring and on A/B runs that isolate the work
item responsible.

## 6. Upscalers and frame generation are disabled

FSR 3/4, DLSS, XeSS, DLAA and frame generation are off in the console profile, and the
native-resolution UWP path excludes the temporal upscalers. The FSR1 shader used by the console
path failed during startup. Enabling any of them needs both a working shader and extra memory that
the budget does not currently have.

## 7. AppContainer restrictions

- `stderr` is not a valid channel; anything written only there is lost.
- `SetThreadDescription` is not in the allowed contract, so guest thread names live in a small
  registry.
- Large section views fail (roughly 768 MiB total), so the runtime uses a private pool with
  commit-on-failure instead.

## 8. Pipeline persistence is only half proven

A separate precompiler was validated: it waits for the warm-up, saves the cache and exits before
the guest starts, with zero compilations afterwards and no assert. What is **not** proven is
persistence across launches and reuse from the game path, and the precompile screen has no visual
menu yet.

## 9. Determinism comparisons are package-specific

The pipeline-sequence hash is the official anchor, but it only compares runs of the same package.
Guest multithreading changes how work is distributed between job-system threads, so raw hashes of
multithreaded modes are not stable; use multiset comparison across repeated runs.

## 10. Console stop endpoint is unreliable

Some Xbox OS versions reject or stall on the package stop endpoint. The stop script checks each
process still belongs to the selected package and then verifies the process is gone; the console
may keep an inactive process record, which is reported separately rather than treated as a
running app.
