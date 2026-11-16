// UWP M5 diagnostic: presents a synthetic frame through the renderer's Swapchain, which on the
// console is backed by the CoreWindow bridge (D3D12 shared image). No Liverpool, no guest data.
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <array>
#include <cstdio>
#include <stdexcept>
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace {
void Check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

int Run() {
    Vulkan::Instance renderer{-1, false};
    const vk::Device device = renderer.GetDevice();
    Check(bool(device), "renderer device unavailable");

    Frontend::WindowSDL window{1280, 720, "Uncharted UWP"};
    Vulkan::Swapchain swapchain{renderer, window};
    std::puts("M5 swapchain: created over the CoreWindow bridge");
    std::fflush(stdout);

    Check(swapchain.AcquireNextImage(), "swapchain acquire failed");
    std::puts("M5 swapchain: image acquired");
    std::fflush(stdout);

    const auto [pool_result, pool] = device.createCommandPoolUnique(
        {.queueFamilyIndex = renderer.GetGraphicsQueueFamilyIndex()});
    Check(pool_result == vk::Result::eSuccess && bool(pool), "swapchain command pool");
    const auto [cmd_result, commands] = device.allocateCommandBuffersUnique(
        {.commandPool = *pool, .level = vk::CommandBufferLevel::ePrimary,
         .commandBufferCount = 1});
    Check(cmd_result == vk::Result::eSuccess && commands.size() == 1, "swapchain command buffer");
    const vk::CommandBuffer command = *commands.front();
    Check(command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) ==
              vk::Result::eSuccess, "swapchain begin");

    constexpr vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 to_general{
        .srcStageMask = vk::PipelineStageFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = swapchain.Image(),
        .subresourceRange = range,
    };
    command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_general});
    const vk::ClearColorValue colour{std::array<float, 4>{0.05f, 0.25f, 0.65f, 1.0f}};
    command.clearColorImage(swapchain.Image(), vk::ImageLayout::eGeneral, colour, range);
    Check(command.end() == vk::Result::eSuccess, "swapchain end");

    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::Semaphore acquired = swapchain.GetImageAcquiredSemaphore();
    const vk::Semaphore ready = swapchain.GetPresentReadySemaphore();
    const vk::SubmitInfo submit{.waitSemaphoreCount = 1,
                                .pWaitSemaphores = &acquired,
                                .pWaitDstStageMask = &wait_stage,
                                .commandBufferCount = 1,
                                .pCommandBuffers = &command,
                                .signalSemaphoreCount = 1,
                                .pSignalSemaphores = &ready};
    Check(renderer.GetGraphicsQueue().submit(std::array{submit}, {}) == vk::Result::eSuccess,
          "swapchain submit failed");
    Check(renderer.GetGraphicsQueue().waitIdle() == vk::Result::eSuccess, "swapchain queue wait");

    Check(swapchain.Present(), "swapchain present failed");
    std::puts("M5 swapchain PASS: renderer swapchain presented a synthetic frame to CoreWindow");
    return 0;
}
} // namespace

extern "C" int bbgpu_uwp_renderer_swapchain_smoke() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::printf("M5 swapchain FAILED: %s\n", error.what());
        return 1;
    }
}
#endif
