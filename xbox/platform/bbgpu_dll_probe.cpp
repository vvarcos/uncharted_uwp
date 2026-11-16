// Renderer DLL probe: constructs the renderer stack inside bbgpu.dll so the console can prove
// the same code runs when loaded from a packaged DLL (the executable keeps the static path).
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <cstdio>
#include <stdexcept>
#include "core/memory.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"

namespace {
int Run() {
    std::puts("M5 dll renderer: entry");
    std::fflush(stdout);
    Vulkan::Instance renderer{-1, false};
    std::puts("M5 dll renderer: instance ready");
    std::fflush(stdout);
    Vulkan::Scheduler scheduler{renderer, false};
    std::puts("M5 dll renderer: scheduler ready");
    std::fflush(stdout);
    Vulkan::Runtime runtime{renderer, scheduler};
    std::puts("M5 dll renderer: runtime ready");
    std::fflush(stdout);
    AmdGpu::Liverpool liverpool{};
    std::puts("M5 dll renderer: liverpool ready");
    std::fflush(stdout);
    Vulkan::Rasterizer rasterizer{renderer, scheduler, runtime, &liverpool};
    std::puts("M5 dll renderer: rasterizer ready");
    std::fflush(stdout);
    std::puts("M5 dll renderer PASS: scheduler, runtime and rasterizer constructed inside bbgpu.dll");
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) int bbgpu_dll_renderer_smoke() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::printf("M5 dll renderer FAILED: %s\n", error.what());
        return 1;
    }
}

// Step 2: copy the executable's already-initialized dispatcher into this module instead of
// re-initializing it (calling the ICD from the DLL crashes), then build the renderer objects.
extern "C" __declspec(dllexport) int bbgpu_dll_host_instance(void* instance_ptr,
                                                             void* dispatcher_ptr,
                                                             void* memory_ptr) {
    if (instance_ptr == nullptr || dispatcher_ptr == nullptr) {
        std::puts("M5 dll host-instance: null argument");
        return 1;
    }
    auto& host_instance = *reinterpret_cast<Vulkan::Instance*>(instance_ptr);
    VULKAN_HPP_DEFAULT_DISPATCHER =
        *reinterpret_cast<const VULKAN_HPP_DEFAULT_DISPATCHER_TYPE*>(dispatcher_ptr);
    Core::Memory::SetHost(reinterpret_cast<Core::MemoryManager*>(memory_ptr));
    std::puts("M5 dll host-instance: dispatcher copied and memory aliased");
    std::fflush(stdout);
    const auto device = host_instance.GetDevice();
    const auto device_handle = static_cast<VkDevice>(device);
    std::printf("M5 dll host-instance: received, device=%p\n",
                reinterpret_cast<void*>(device_handle));
    std::fflush(stdout);
    try {
        Vulkan::Scheduler scheduler{host_instance, false};
        std::puts("M5 dll host-instance: scheduler ready");
        std::fflush(stdout);
        Vulkan::Runtime runtime{host_instance, scheduler};
        std::puts("M5 dll host-instance: runtime ready");
        std::fflush(stdout);
        std::puts("M5 dll host-instance: probing memory singleton");
        std::fflush(stdout);
        auto* memory = Core::Memory::Instance();
        std::printf("M5 dll host-instance: memory=%p\n", static_cast<void*>(memory));
        std::fflush(stdout);
        AmdGpu::Liverpool liverpool{};
        std::puts("M5 dll host-instance: liverpool ready");
        std::fflush(stdout);
        Vulkan::Rasterizer rasterizer{host_instance, scheduler, runtime, &liverpool};
        std::puts("M5 dll host-instance PASS: rasterizer built in the DLL");
    } catch (const std::exception& error) {
        std::printf("M5 dll host-instance FAILED: %s\n", error.what());
        return 1;
    }
    return 0;
}
#endif
