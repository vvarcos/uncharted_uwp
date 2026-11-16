// M5 diagnostic: draw and read back with the real renderer Vulkan::Instance device.
// This never starts Liverpool, the guest, CoreWindow presentation, or a flip event.
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "video_core/renderer_vulkan/vk_instance.h"
#include <vk_mem_alloc.h>
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/m5_offscreen_frag.h"

namespace {
constexpr uint32_t Width = 64, Height = 64;
constexpr auto Format = vk::Format::eR8G8B8A8Unorm;

struct ImageAllocation {
    VmaAllocator allocator{};
    VkImage image{};
    VmaAllocation allocation{};
    ~ImageAllocation() { if (image) vmaDestroyImage(allocator, image, allocation); }
};
struct BufferAllocation {
    VmaAllocator allocator{};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    ~BufferAllocation() { if (buffer) vmaDestroyBuffer(allocator, buffer, allocation); }
};
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int DrawAndReadback() {
    Vulkan::Instance renderer{-1, false};
    const vk::Device device = renderer.GetDevice();
    const VmaAllocator allocator = renderer.GetAllocator();
    Require(device && allocator, "renderer class device/VMA unavailable");

    ImageAllocation image{.allocator = allocator};
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {Width, Height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo image_alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    Require(vmaCreateImage(allocator, &image_info, &image_alloc, &image.image,
                           &image.allocation, nullptr) == VK_SUCCESS, "VMA offscreen image failed");

    BufferAllocation readback{.allocator = allocator};
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = Width * Height * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo buffer_alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    Require(vmaCreateBuffer(allocator, &buffer_info, &buffer_alloc, &readback.buffer,
                            &readback.allocation, nullptr) == VK_SUCCESS, "VMA readback buffer failed");

    const vk::ImageViewCreateInfo view_info{
        .image = vk::Image{image.image},
        .viewType = vk::ImageViewType::e2D,
        .format = Format,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    const auto [view_result, view] = device.createImageViewUnique(view_info);
    Require(view_result == vk::Result::eSuccess && bool(view), "offscreen image view failed");
    const auto [vs_result, vs] = device.createShaderModuleUnique(
        {.codeSize = sizeof(FS_TRI_VERT), .pCode = FS_TRI_VERT});
    Require(vs_result == vk::Result::eSuccess && bool(vs), "vertex shader module failed");
    const auto [fs_result, fs] = device.createShaderModuleUnique(
        {.codeSize = sizeof(M5_OFFSCREEN_FRAG), .pCode = M5_OFFSCREEN_FRAG});
    Require(fs_result == vk::Result::eSuccess && bool(fs), "fragment shader module failed");
    const auto [layout_result, layout] = device.createPipelineLayoutUnique({});
    Require(layout_result == vk::Result::eSuccess && bool(layout), "offscreen pipeline layout failed");

    const std::array stages{
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eVertex,
                                          .module = *vs, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{.stage = vk::ShaderStageFlagBits::eFragment,
                                          .module = *fs, .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering_pipeline{
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &Format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1, .scissorCount = 1,
    };
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState color_blend{
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo blending{
        .attachmentCount = 1, .pAttachments = &color_blend,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<uint32_t>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &rendering_pipeline,
        .stageCount = static_cast<uint32_t>(stages.size()), .pStages = stages.data(),
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_state, .pRasterizationState = &raster,
        .pMultisampleState = &multisample, .pColorBlendState = &blending,
        .pDynamicState = &dynamic, .layout = *layout,
    };
    const auto [pipeline_result, pipeline] = device.createGraphicsPipelineUnique({}, pipeline_info);
    Require(pipeline_result == vk::Result::eSuccess && bool(pipeline), "offscreen pipeline failed");

    const auto [pool_result, pool] = device.createCommandPoolUnique(
        {.queueFamilyIndex = renderer.GetGraphicsQueueFamilyIndex()});
    Require(pool_result == vk::Result::eSuccess && bool(pool), "offscreen command pool failed");
    const auto [cmd_result, buffers] = device.allocateCommandBuffersUnique(
        {.commandPool = *pool, .level = vk::CommandBufferLevel::ePrimary,
         .commandBufferCount = 1});
    Require(cmd_result == vk::Result::eSuccess && buffers.size() == 1,
            "offscreen command buffer failed");
    const vk::CommandBuffer cmd = *buffers.front();
    Require(cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) ==
                vk::Result::eSuccess, "offscreen record begin failed");
    const vk::ImageMemoryBarrier to_color{
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = vk::Image{image.image},
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                        vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, {}, {},
                        std::array{to_color});
    vk::ClearValue clear{};
    clear.color.float32[0] = 1.0f;
    clear.color.float32[3] = 1.0f;
    const vk::RenderingAttachmentInfo attachment{
        .imageView = *view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = clear,
    };
    const vk::RenderingInfo rendering{
        .renderArea = {{0, 0}, {Width, Height}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };
    cmd.beginRenderingKHR(rendering);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);
    cmd.setViewport(0, std::array{vk::Viewport{0, 0, float(Width), float(Height), 0, 1}});
    cmd.setScissor(0, std::array{vk::Rect2D{{0, 0}, {Width / 2, Height}}});
    cmd.draw(3, 1, 0, 0);
    cmd.endRenderingKHR();
    const vk::ImageMemoryBarrier to_copy{
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
        .image = vk::Image{image.image},
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, std::array{to_copy});
    const vk::BufferImageCopy region{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {Width, Height, 1},
    };
    cmd.copyImageToBuffer(vk::Image{image.image}, vk::ImageLayout::eTransferSrcOptimal,
                          vk::Buffer{readback.buffer}, std::array{region});
    const vk::BufferMemoryBarrier to_host{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eHostRead,
        .buffer = vk::Buffer{readback.buffer},
        .size = VK_WHOLE_SIZE,
    };
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eHost, {}, {}, std::array{to_host}, {});
    Require(cmd.end() == vk::Result::eSuccess, "offscreen record end failed");
    const auto [fence_result, fence] = device.createFenceUnique({});
    Require(fence_result == vk::Result::eSuccess && bool(fence), "offscreen fence failed");
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
    Require(renderer.GetGraphicsQueue().submit(std::array{submit}, *fence) ==
                vk::Result::eSuccess, "offscreen submit failed");
    Require(device.waitForFences(std::array{*fence}, VK_TRUE, 5000000000ull) ==
                vk::Result::eSuccess, "offscreen fence timeout/failure");
    Require(vmaInvalidateAllocation(allocator, readback.allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
            "offscreen cache invalidate failed");
    void* mapped = nullptr;
    Require(vmaMapMemory(allocator, readback.allocation, &mapped) == VK_SUCCESS && mapped,
            "offscreen readback map failed");
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    const auto green = bytes + 4 * (16 * Width + 16);
    const auto red = bytes + 4 * (16 * Width + 48);
    const std::array<uint8_t, 4> left{green[0], green[1], green[2], green[3]};
    const std::array<uint8_t, 4> right{red[0], red[1], red[2], red[3]};
    vmaUnmapMemory(allocator, readback.allocation);
    std::printf("M5 renderer offscreen readback: left=%02X%02X%02X%02X right=%02X%02X%02X%02X\n",
                left[0], left[1], left[2], left[3], right[0], right[1], right[2], right[3]);
    Require(left == std::array<uint8_t, 4>{0, 255, 0, 255} &&
            right == std::array<uint8_t, 4>{255, 0, 0, 255},
            "offscreen triangle/clear pixel mismatch");
    std::puts("M5 renderer offscreen draw/readback PASS: real class device; no guest frame/gameplay");
    return 0;
}
} // namespace

extern "C" int bbgpu_uwp_renderer_offscreen_smoke(void) {
    try { return DrawAndReadback(); }
    catch (const std::exception& error) {
        std::printf("M5 renderer offscreen draw/readback FAILED: %s\n", error.what());
        return 1;
    }
}
#endif
