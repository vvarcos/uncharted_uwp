// UWP renderer-in-DLL probe: builds the renderer objects inside bbprobe.dll, so the console
// loads the heavy code from a packaged DLL instead of the (size-limited) main image.
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <cstdio>
#include <stdexcept>
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#ifdef UNCHARTED_UWP
#include <windows.h>
#endif

namespace {
int Run() {
    std::puts("M5 dll boot: entry");
    std::fflush(stdout);
#ifdef UNCHARTED_UWP
    // The DLL has its own Vulkan-Hpp dispatcher: initialize it with the packaged Dozen ICD
    // entry point before creating anything (the executable's dispatcher is a different one).
    static HMODULE dozen = LoadPackagedLibrary(L"vulkan_dzn.dll", 0);
    if (!dozen) {
        std::printf("M5 dll boot FAILED: Dozen load error=%lu\n", GetLastError());
        return 1;
    }
    const auto icd = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(dozen, "vk_icdGetInstanceProcAddr"));
    if (!icd) {
        std::puts("M5 dll boot FAILED: Dozen ICD entry point missing");
        return 1;
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(icd);
    std::puts("M5 dll boot: dispatcher ready");
    std::fflush(stdout);
#endif
    Vulkan::Instance renderer{-1, false};
    std::puts("M5 dll boot: instance ready");
    std::fflush(stdout);
    Vulkan::Scheduler scheduler{renderer, false};
    std::puts("M5 dll boot: scheduler ready");
    std::fflush(stdout);
    Vulkan::Runtime runtime{renderer, scheduler};
    std::puts("M5 dll boot: runtime ready");
    std::fflush(stdout);
    AmdGpu::Liverpool liverpool{};
    std::puts("M5 dll boot: liverpool ready");
    std::fflush(stdout);
    Vulkan::Rasterizer rasterizer{renderer, scheduler, runtime, &liverpool};
    std::puts("M5 dll boot: rasterizer ready");
    std::fflush(stdout);
    std::puts("M5 dll boot PASS: scheduler, runtime and rasterizer constructed inside the DLL");
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) int bbgpu_dll_boot_smoke() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::printf("M5 dll boot FAILED: %s\n", error.what());
        return 1;
    }
}
#endif
