// UWP M5 minimal probe: constructs only the renderer's timeline semaphore, to isolate whether
// that object (and what it pulls from the Vulkan-Hpp/CRT layers) is what stops the package from
// running. No <thread>, no resource pool, no scheduler.
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <cstdio>
#include <stdexcept>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace {
int Run() {
    Vulkan::Instance renderer{-1, false};
    std::puts("M5 sem: instance ready");
    std::fflush(stdout);
    {
        Vulkan::Semaphore semaphore{renderer};
        std::puts("M5 sem PASS: timeline semaphore created");
        std::fflush(stdout);
    }
    std::puts("M5 sem PASS: released");
    return 0;
}
} // namespace

extern "C" int bbgpu_uwp_renderer_sem_smoke() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::printf("M5 sem FAILED: %s\n", error.what());
        return 1;
    }
}
#endif
