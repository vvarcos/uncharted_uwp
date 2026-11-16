// UWP M5 diagnostic: runs the host post-processing pass on the real Vulkan device with the
// descriptor-set fallback used when VK_KHR_push_descriptor is unavailable. It does NOT touch
// Liverpool, guest memory or VideoOut, and the image it renders is a synthetic clear.
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "video_core/renderer_vulkan/vk_instance.h"
#include <vk_mem_alloc.h>
#include "video_core/renderer_vulkan/host_passes/fsr_pass.h"
#include "video_core/renderer_vulkan/host_passes/pp_pass.h"
#include "video_core/host_shaders/m5_empty_compute_comp.h"
#include "video_core/host_shaders/fsr_easu_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

namespace {
constexpr u32 Size = 64;
constexpr u32 SourceSize = 32;

// Declared directly: the probe must compile even where <windows.h> is kept out of the way.
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentDirectoryW(unsigned long,
                                                                             wchar_t*);

void Check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

struct Image {
    VkImage image{};
    VmaAllocation allocation{};
    vk::UniqueImageView view{};

    void Create(VmaAllocator allocator, vk::Device device, vk::Format format,
                vk::ImageUsageFlags usage, const char* name, u32 extent = Size) {
        const VkImageCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = static_cast<VkFormat>(format),
            .extent = {extent, extent, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = static_cast<VkImageUsageFlags>(usage),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        const VmaAllocationCreateInfo alloc_ci{
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        };
        Check(vmaCreateImage(allocator, &ci, &alloc_ci, &image, &allocation, nullptr) == VK_SUCCESS,
              "host pass image allocation failed");
        auto [view_result, created_view] = device.createImageViewUnique({
            .image = vk::Image{image},
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        Check(view_result == vk::Result::eSuccess && bool(created_view),
              "host pass image view failed");
        view = std::move(created_view);
        std::printf("M5 host pass image ready: %s\n", name);
    }

    void Destroy(VmaAllocator allocator, vk::Device device) {
        view.reset();
        if (image) {
            vmaDestroyImage(allocator, image, allocation);
            image = VK_NULL_HANDLE;
            allocation = VK_NULL_HANDLE;
        }
        (void)device;
    }
};

int Run() {
    // Import causality probe: this call alone pulls api-ms-win-core-processenvironment-l1-1-0
    // (GetCurrentDirectoryW). If the package still activates, that import is not the blocker.
    {
        wchar_t cwd[8]{};
        const unsigned long n = GetCurrentDirectoryW(8, cwd);
        std::printf("M5 cwd probe: %lu characters\n", n);
        std::fflush(stdout);
    }
    Vulkan::Instance renderer{-1, false};
    const vk::Device device = renderer.GetDevice();
    const VmaAllocator allocator = renderer.GetAllocator();
    const vk::Queue queue = renderer.GetGraphicsQueue();
    Check(device && allocator, "renderer device/VMA unavailable");

    Image source;
    source.Create(allocator, device, vk::Format::eR8G8B8A8Unorm,
                  vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, "source",
                  SourceSize);
    Image target;
    target.Create(allocator, device, vk::Format::eR8G8B8A8Unorm,
                  vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc |
                      vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                  "post-process target");

    Vulkan::HostPasses::PostProcessingPass pass;
    pass.Create(device, vk::Format::eR8G8B8A8Unorm, 1);
    std::puts("M5 host pass created: layout, pipeline and descriptor sets");

    const auto [pool_result, command_pool] = device.createCommandPoolUnique(
        {.queueFamilyIndex = renderer.GetGraphicsQueueFamilyIndex()});
    Check(pool_result == vk::Result::eSuccess && bool(command_pool), "host pass command pool");
    const auto [cmd_result, commands] = device.allocateCommandBuffersUnique(
        {.commandPool = *command_pool, .level = vk::CommandBufferLevel::ePrimary,
         .commandBufferCount = 1});
    Check(cmd_result == vk::Result::eSuccess && commands.size() == 1, "host pass command buffer");
    const vk::CommandBuffer command = *commands.front();
    Check(command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) ==
              vk::Result::eSuccess, "host pass begin");

    // Isolated push-constant probe: the FSR compute pipeline aborts the process right after its
    // layout is created. This creates a compute pipeline with the same kind of push-constant
    // range (80 bytes) and a shader that does not read it, to tell layout apart from shader use.
    {
        const vk::PushConstantRange constant_range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                                   .offset = 0,
                                                   .size = 80};
        std::puts("M5 pushconst layout: creating");
        std::fflush(stdout);
        const auto [layout_result, probe_layout] = device.createPipelineLayoutUnique(
            {.pushConstantRangeCount = 1, .pPushConstantRanges = &constant_range});
        Check(layout_result == vk::Result::eSuccess && bool(probe_layout),
              "pushconst pipeline layout failed");
        std::puts("M5 pushconst layout: ok");
        std::fflush(stdout);
        const auto [module_result, module] = device.createShaderModuleUnique(
            {.codeSize = sizeof(M5_EMPTY_COMPUTE_COMP), .pCode = M5_EMPTY_COMPUTE_COMP});
        Check(module_result == vk::Result::eSuccess && bool(module), "empty compute module failed");
        std::puts("M5 pushconst pipeline: creating");
        std::fflush(stdout);
        const auto [pipeline_result, probe_pipeline] = device.createComputePipelineUnique(
            {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *module,
                           .pName = "main"},
                 .layout = *probe_layout});
        Check(pipeline_result == vk::Result::eSuccess && bool(probe_pipeline),
              "pushconst compute pipeline failed");
        std::puts("M5 pushconst pipeline: ok");
        std::fflush(stdout);
    }

    // Second probe: the FSR descriptor layout (immutable sampler + storage image) and push
    // constants, but a shader that does not read any binding. Separates layout from shader use.
    {
        const auto [probe_sampler_result, probe_sampler] = device.createSamplerUnique(
            {.magFilter = vk::Filter::eLinear, .minFilter = vk::Filter::eLinear});
        Check(probe_sampler_result == vk::Result::eSuccess && bool(probe_sampler),
              "probe sampler failed");
        const std::array probe_bindings{
            vk::DescriptorSetLayoutBinding{.binding = 0,
                                           .descriptorType = vk::DescriptorType::eSampledImage,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{.binding = 1,
                                           .descriptorType = vk::DescriptorType::eStorageImage,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eCompute},
            vk::DescriptorSetLayoutBinding{.binding = 2,
                                           .descriptorType = vk::DescriptorType::eSampler,
                                           .descriptorCount = 1,
                                           .stageFlags = vk::ShaderStageFlagBits::eCompute,
                                           .pImmutableSamplers = &probe_sampler.get()},
        };
        const auto [set_layout_result, probe_set_layout] = device.createDescriptorSetLayoutUnique(
            {.bindingCount = static_cast<u32>(probe_bindings.size()),
             .pBindings = probe_bindings.data()});
        Check(set_layout_result == vk::Result::eSuccess && bool(probe_set_layout),
              "probe descriptor set layout failed");
        std::puts("M5 fsr layout probe: set layout ok");
        std::fflush(stdout);
        const vk::PushConstantRange constant_range{.stageFlags = vk::ShaderStageFlagBits::eCompute,
                                                   .offset = 0,
                                                   .size = 80};
        const vk::DescriptorSetLayout raw_set = *probe_set_layout;
        const auto [probe_layout_result, probe_pipeline_layout] = device.createPipelineLayoutUnique(
            {.setLayoutCount = 1,
             .pSetLayouts = &raw_set,
             .pushConstantRangeCount = 1,
             .pPushConstantRanges = &constant_range});
        Check(probe_layout_result == vk::Result::eSuccess && bool(probe_pipeline_layout),
              "probe pipeline layout failed");
        std::puts("M5 fsr layout probe: pipeline layout ok");
        std::fflush(stdout);
        const auto [probe_module_result, probe_module] = device.createShaderModuleUnique(
            {.codeSize = sizeof(M5_EMPTY_COMPUTE_COMP), .pCode = M5_EMPTY_COMPUTE_COMP});
        Check(probe_module_result == vk::Result::eSuccess && bool(probe_module),
              "probe empty module failed");
        std::puts("M5 fsr layout probe: pipeline creating");
        std::fflush(stdout);
        const auto [probe_pipe_result, probe_pipe] = device.createComputePipelineUnique(
            {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *probe_module,
                           .pName = "main"},
                 .layout = *probe_pipeline_layout});
        Check(probe_pipe_result == vk::Result::eSuccess && bool(probe_pipe),
              "probe pipeline with fsr layout failed");
        std::puts("M5 fsr layout probe: pipeline ok");
        std::fflush(stdout);
    }

    constexpr vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::ImageMemoryBarrier2 to_clear{
        .srcStageMask = vk::PipelineStageFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eClear,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = vk::Image{source.image},
        .subresourceRange = range,
    };
    command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_clear});
    const vk::ClearColorValue green{std::array<float, 4>{0.0f, 1.0f, 0.0f, 1.0f}};
    command.clearColorImage(vk::Image{source.image}, vk::ImageLayout::eTransferDstOptimal, green,
                            range);
    const vk::ImageMemoryBarrier2 to_sample{
        .srcStageMask = vk::PipelineStageFlagBits2::eClear,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = vk::Image{source.image},
        .subresourceRange = range,
    };
    command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_sample});


    // FSR is disabled on UWP: the guest's 1920x1080 frame is larger than the 1280x720 CoreWindow
    // surface, so the upscaler is never used, and creating its EASU pipeline crashes the driver.
    Vulkan::Frame frame{};
    frame.width = Size;
    frame.height = Size;
    frame.image = vk::Image{target.image};
    frame.image_view = *target.view;
    Vulkan::HostPasses::PostProcessingPass::Settings settings{};
    settings.gamma = 1.0f;
    pass.Render(command, *source.view, {SourceSize, SourceSize}, frame, settings, 0u);
    std::puts("M5 host pass recorded: descriptor set bound and draw issued");

    const vk::ImageMemoryBarrier2 to_copy{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .image = vk::Image{target.image},
        .subresourceRange = range,
    };
    command.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_copy});

    const VkBufferCreateInfo readback_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = Size * Size * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo readback_alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer readback{};
    VmaAllocation readback_allocation{};
    Check(vmaCreateBuffer(allocator, &readback_ci, &readback_alloc, &readback, &readback_allocation,
                          nullptr) == VK_SUCCESS, "host pass readback buffer");
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {Size, Size, 1},
    };
    command.copyImageToBuffer(vk::Image{target.image}, vk::ImageLayout::eTransferSrcOptimal,
                              vk::Buffer{readback}, region);
    Check(command.end() == vk::Result::eSuccess, "host pass end");

    const auto [fence_result, fence] = device.createFenceUnique({});
    Check(fence_result == vk::Result::eSuccess && bool(fence), "host pass fence");
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &command};
    Check(queue.submit(std::array{submit}, *fence) == vk::Result::eSuccess, "host pass submit");
    Check(device.waitForFences(std::array{*fence}, VK_TRUE, 10000000000ull) ==
              vk::Result::eSuccess, "host pass fence timeout");

    void* mapped = nullptr;
    Check(vmaMapMemory(allocator, readback_allocation, &mapped) == VK_SUCCESS && mapped,
          "host pass readback map");
    const auto* pixels = static_cast<const u8*>(mapped);
    const u32 center = ((Size / 2) * Size + (Size / 2)) * 4;
    std::printf("M5 host pass readback centre: %u %u %u %u\n", pixels[center + 0],
                pixels[center + 1], pixels[center + 2], pixels[center + 3]);
    const bool is_green =
        pixels[center + 1] > 200 && pixels[center + 0] < 40 && pixels[center + 2] < 40;
    vmaUnmapMemory(allocator, readback_allocation);
    vmaDestroyBuffer(allocator, readback, readback_allocation);
    target.Destroy(allocator, device);
    source.Destroy(allocator, device);

    Check(is_green, "post-process output is not the cleared green input");
    std::puts("M5 host pass PASS: descriptor sets, push constants and dynamic rendering on device; "
              "synthetic 64x64 image, no guest frame");
    return 0;
}
} // namespace

extern "C" int bbgpu_uwp_renderer_hostpass_smoke() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::printf("M5 host pass FAILED: %s\n", error.what());
        return 1;
    }
}
#endif
