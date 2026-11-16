#if UNCHARTED_PROBE_LEVEL < 1
#include <cstdio>
extern "C" int bbgpu_uwp_renderer_probe_smoke() { return 0; }
#else
// UWP M5 probe ladder: builds one renderer primitive at a time (selected at configure time by
// UNCHARTED_PROBE_LEVEL) to find the first piece that stops the package from activating.
// 1 = timeline semaphore, 2 = + command pool, 3 = + std::jthread, 4 = + scheduler command buffers.
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <cstdio>
#include <stdexcept>
#include <thread>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#if UNCHARTED_PROBE_LEVEL == 9
#include "video_core/amdgpu/liverpool.h"
#endif

namespace {
int Run() {
#if UNCHARTED_PROBE_LEVEL == 9
    // Liverpool only: separates "its static initializers/CRT needs" from image size.
    AmdGpu::Liverpool liverpool{};
    std::puts("M5 probe (level 9): liverpool object ready");
    std::fflush(stdout);
    std::puts("M5 probe PASS (level 9): liverpool constructed");
    return 0;
#endif
    Vulkan::Instance renderer{-1, false};
    std::printf("M5 probe (level %d): instance ready\n", UNCHARTED_PROBE_LEVEL);
    std::fflush(stdout);

    Vulkan::Semaphore semaphore{renderer};
    std::printf("M5 probe (level %d): timeline semaphore ready\n", UNCHARTED_PROBE_LEVEL);
    std::fflush(stdout);

#if UNCHARTED_PROBE_LEVEL >= 2
    Vulkan::CommandPool pool{renderer, &semaphore};
    std::printf("M5 probe (level %d): command pool ready\n", UNCHARTED_PROBE_LEVEL);
    std::fflush(stdout);
#endif
#if UNCHARTED_PROBE_LEVEL >= 3
    {
        std::jthread worker{[](std::stop_token token) {
            while (!token.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }};
        std::printf("M5 probe (level %d): jthread started\n", UNCHARTED_PROBE_LEVEL);
        std::fflush(stdout);
    }
#endif
    std::printf("M5 probe PASS (level %d): primitives constructed\n", UNCHARTED_PROBE_LEVEL);
    return 0;
}
} // namespace

extern "C" int bbgpu_uwp_renderer_probe_smoke() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::printf("M5 probe FAILED (level %d): %s\n", UNCHARTED_PROBE_LEVEL, error.what());
        return 1;
    }
}
#endif
#endif
