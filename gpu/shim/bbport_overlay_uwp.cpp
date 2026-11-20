// SPDX-License-Identifier: GPL-2.0-or-later
// UWP: the in-game menu is desktop-only (SDL window, SDL events, ImGui backend). This file keeps
// the RAM/FPS telemetry of that menu and draws it as a two-line HUD straight into the swapchain
// image the presenter leaves in ColorAttachmentOptimal: one dynamic-rendering pass, one pipeline,
// one host-visible vertex buffer per presentation slot, one atlas baked once with imstb_truetype
// over bb_font_ttf, and one draw call. No ImGui, no SDL, no per-frame allocation.
#ifdef UNCHARTED_UWP
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <winrt/Windows.System.h>

#include "bbport_overlay.h"
#include "common/bbport_counters.h"

// vk_mem_alloc.h pulls the C vulkan.h first; including it after vk_common.h (which brings the
// Vulkan-Hpp headers this target uses) keeps a single, matching header set.
#include <vk_mem_alloc.h>

// This is the only translation unit that emits the stb_truetype implementation with external
// linkage (button_prompts.cpp builds it as STBTT_STATIC, so its copy stays private there).
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#include "video_core/host_shaders/hud_frag.h"
#include "video_core/host_shaders/hud_vert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

extern "C" const unsigned char bb_font_ttf[];

namespace BbOverlay {
namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point last_present{};
Clock::time_point last_report{};
double frame_ms_avg{};
u64 memory_peak_mib{};
u64 memory_current_mib{};
u64 memory_limit_mib{};
u64 present_count{};
bool hud_enabled{true};
BbPort::Counters::Frame frame_counters{};

// ---------------------------------------------------------------------------------------------
// HUD resources. Everything is created once in Init; Render only rewrites the mapped vertices.
// ---------------------------------------------------------------------------------------------
constexpr u32 AtlasColumns = 16;
constexpr u32 AtlasRows = 6; // 96 cells, 95 printable ASCII glyphs (32..126)
constexpr u32 CellWidth = 24;
constexpr u32 CellHeight = 28;
constexpr u32 AtlasWidth = AtlasColumns * CellWidth;  // 384
constexpr u32 AtlasHeight = AtlasRows * CellHeight;   // 168
constexpr u32 AtlasBytes = AtlasWidth * AtlasHeight;  // 64512 (63 KiB, R8)
constexpr u32 PadX = 2;      // left margin inside a cell, absorbs negative bearings
constexpr u32 Baseline = 22; // baseline row inside a cell
constexpr float FontPixelHeight = 21.0f;
constexpr u32 MaxCharsPerLine = 32;
constexpr u32 MaxLines = 4;
constexpr u32 MaxGlyphs = MaxCharsPerLine * MaxLines;
constexpr u32 VerticesPerGlyph = 6;
constexpr u32 MaxVertices = MaxGlyphs * VerticesPerGlyph;
constexpr u32 MaxImageSlots = 8;
constexpr float MarginPx = 16.0f;
constexpr float LineSpacingPx = 26.0f;

struct HudVertex {
    float x, y;    // clip space
    float u, v;    // atlas
    float r, g, b, a;
};
static_assert(sizeof(HudVertex) == 32, "HUD vertex must stay 32 bytes");

struct HudGlyph {
    u16 cell;   // atlas cell index
    u8 width;   // bitmap width in pixels
    u8 height;  // bitmap height in pixels
    s16 xoff;   // bitmap left bearing
    s16 yoff;   // bitmap top bearing relative to the baseline
    float advance;
};

HudGlyph hud_glyphs[95]{};

struct HudSlot {
    VkBuffer buffer{};
    VmaAllocation allocation{};
    HudVertex* mapped{};
    VkImageView view{}; // swapchain view this slot belongs to, assigned on first use
};
HudSlot hud_slots[MaxImageSlots]{};
u32 hud_slot_count{}; // buffers allocated (= min(image_count, MaxImageSlots))
u32 hud_used_slots{}; // views seen so far
u32 hud_round_robin{};

bool hud_ready{};
vk::Device hud_device{};
VmaAllocator hud_allocator{};
vk::Pipeline hud_pipeline{};
vk::PipelineLayout hud_pipeline_layout{};
vk::DescriptorSetLayout hud_descriptor_set_layout{};
vk::DescriptorPool hud_descriptor_pool{};
vk::DescriptorSet hud_descriptor_set{};
vk::Sampler hud_sampler{};
vk::ImageView hud_atlas_view{};
VkImage hud_atlas_image{};
VmaAllocation hud_atlas_allocation{};

/// Vulkan-Hpp runs without exceptions here: unwrap the result, reporting the failing step.
template <typename T>
T HudCheck(vk::ResultValue<T> result, const char* what) {
    if (result.result != vk::Result::eSuccess) {
        std::printf("HUD: visual overlay: %s failed\n", what);
        return T{};
    }
    return std::move(result.value);
}

/// Bakes the 95 printable ASCII glyphs into `atlas` (AtlasBytes, R8, rows top to bottom).
void BakeAtlas(u8* atlas) {
    stbtt_fontinfo font{};
    if (!stbtt_InitFont(&font, bb_font_ttf, stbtt_GetFontOffsetForIndex(bb_font_ttf, 0))) {
        return;
    }
    const float scale = stbtt_ScaleForPixelHeight(&font, FontPixelHeight);
    for (u32 i = 0; i < 95; ++i) {
        const int cp = int(32 + i);
        HudGlyph& glyph = hud_glyphs[i];
        glyph = HudGlyph{.cell = static_cast<u16>(i)};
        int advance = 0, bearing = 0;
        stbtt_GetCodepointHMetrics(&font, cp, &advance, &bearing);
        glyph.advance = static_cast<float>(advance) * scale;

        int w = 0, h = 0, xoff = 0, yoff = 0;
        unsigned char* bitmap = stbtt_GetCodepointBitmap(&font, scale, scale, cp, &w, &h, &xoff,
                                                         &yoff);
        if (!bitmap) {
            continue;
        }
        const int cell_x = int(i % AtlasColumns) * int(CellWidth);
        const int cell_y = int(i / AtlasColumns) * int(CellHeight);
        glyph.width = static_cast<u8>(std::min(w, int(CellWidth)));
        glyph.height = static_cast<u8>(std::min(h, int(CellHeight)));
        glyph.xoff = static_cast<s16>(xoff);
        glyph.yoff = static_cast<s16>(yoff);
        for (int y = 0; y < h; ++y) {
            const int dy = cell_y + int(Baseline) + yoff + y;
            if (dy < cell_y || dy >= cell_y + int(CellHeight)) {
                continue;
            }
            for (int x = 0; x < w; ++x) {
                const int dx = cell_x + int(PadX) + xoff + x;
                if (dx < cell_x || dx >= cell_x + int(CellWidth)) {
                    continue;
                }
                u8& dst = atlas[dy * int(AtlasWidth) + dx];
                dst = std::max(dst, bitmap[y * w + x]);
            }
        }
        stbtt_FreeBitmap(bitmap, nullptr);
    }
}

/// Copies a full R8 staging buffer into the atlas image and leaves it shader-readable.
bool UploadAtlas(vk::Device device, vk::Queue queue, u32 queue_family, VkBuffer staging) {
    const vk::CommandPool pool = HudCheck(
        device.createCommandPool({.queueFamilyIndex = queue_family}), "command pool");
    if (!pool) {
        return false;
    }
    const auto command_buffers = HudCheck(
        device.allocateCommandBuffers({.commandPool = pool,
                                       .level = vk::CommandBufferLevel::ePrimary,
                                       .commandBufferCount = 1}),
        "command buffer");
    if (command_buffers.empty()) {
        return false;
    }
    const vk::CommandBuffer cmd = command_buffers.front();
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

    vk::ImageMemoryBarrier to_transfer{
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .image = vk::Image{hud_atlas_image},
        .subresourceRange = range,
    };
    const vk::BufferImageCopy region{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {AtlasWidth, AtlasHeight, 1},
    };
    vk::ImageMemoryBarrier to_shader{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .image = vk::Image{hud_atlas_image},
        .subresourceRange = range,
    };

    cmd.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, std::array{to_transfer});
    cmd.copyBufferToImage(vk::Buffer{staging}, vk::Image{hud_atlas_image},
                          vk::ImageLayout::eTransferDstOptimal, std::array{region});
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                        vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
                        std::array{to_shader});
    cmd.end();

    const vk::Fence fence = HudCheck(device.createFence({}), "fence");
    if (!fence) {
        device.destroyCommandPool(pool);
        return false;
    }
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cmd};
    queue.submit(std::array{submit}, fence);
    const vk::Result waited = device.waitForFences(std::array{fence}, VK_TRUE, 5'000'000'000ull);
    device.destroyFence(fence);
    device.destroyCommandPool(pool);
    return waited == vk::Result::eSuccess;
}

/// Creates the pipeline, atlas, descriptor set and one vertex buffer per presentation slot.
bool CreateResources(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    const vk::Device device = instance.GetDevice();
    const VmaAllocator allocator = instance.GetAllocator();
    if (!device || !allocator || format == vk::Format::eUndefined) {
        std::puts("HUD: visual overlay skipped (device/format unavailable)");
        return false;
    }
    if (hud_ready) {
        if (hud_device == device) {
            return true;
        }
        // A different device: the old handles may already be invalid. Drop them without touching
        // the old device and rebuild; this leaks only on a presenter rebuild.
        hud_ready = false;
        hud_atlas_view = vk::ImageView();
        hud_sampler = vk::Sampler();
        hud_descriptor_pool = vk::DescriptorPool();
        hud_descriptor_set = vk::DescriptorSet();
        hud_descriptor_set_layout = vk::DescriptorSetLayout();
        hud_pipeline_layout = vk::PipelineLayout();
        hud_pipeline = vk::Pipeline();
        hud_atlas_image = {};
        hud_atlas_allocation = {};
        hud_slot_count = 0;
        hud_used_slots = 0;
        for (HudSlot& slot : hud_slots) {
            slot = {};
        }
    }

    hud_device = device;
    hud_allocator = allocator;

    // Shaders generated from host_shaders/hud.{vert,frag} (registered under UNCHARTED_UWP).
    const vk::ShaderModule vertex_module = Vulkan::CompileSPV(HUD_VERT, device);
    const vk::ShaderModule fragment_module = Vulkan::CompileSPV(HUD_FRAG, device);
    if (!vertex_module || !fragment_module) {
        std::puts("HUD: visual overlay skipped (shader compile failed)");
        return false;
    }

    const vk::DescriptorSetLayoutBinding atlas_binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    hud_descriptor_set_layout = HudCheck(
        device.createDescriptorSetLayout({
            .bindingCount = 1,
            .pBindings = &atlas_binding,
        }),
        "descriptor set layout");
    if (!hud_descriptor_set_layout) {
        return false;
    }

    hud_pipeline_layout = HudCheck(
        device.createPipelineLayout({
            .setLayoutCount = 1,
            .pSetLayouts = &hud_descriptor_set_layout,
        }),
        "pipeline layout");
    if (!hud_pipeline_layout) {
        return false;
    }

    const std::array shader_stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vertex_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fragment_module,
            .pName = "main",
        },
    };

    const vk::VertexInputBindingDescription vertex_binding{
        .binding = 0,
        .stride = sizeof(HudVertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };
    const std::array vertex_attributes{
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(HudVertex, x),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(HudVertex, u),
        },
        vk::VertexInputAttributeDescription{
            .location = 2,
            .binding = 0,
            .format = vk::Format::eR32G32B32A32Sfloat,
            .offset = offsetof(HudVertex, r),
        },
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &vertex_binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(vertex_attributes.size()),
        .pVertexAttributeDescriptions = vertex_attributes.data(),
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState color_attachment{
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eOne,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = false,
        .logicOp = vk::LogicOp::eCopy,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
    };
    const std::array dynamic_states{
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };
    const vk::PipelineDynamicStateCreateInfo dynamic_state{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::PipelineRenderingCreateInfo pipeline_rendering{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &pipeline_rendering,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = hud_pipeline_layout,
    };
    hud_pipeline = HudCheck(device.createGraphicsPipeline({}, pipeline_info), "graphics pipeline");
    device.destroyShaderModule(vertex_module);
    device.destroyShaderModule(fragment_module);
    if (!hud_pipeline) {
        std::puts("HUD: visual overlay skipped (pipeline create failed)");
        return false;
    }

    // Atlas image (R8) + its staging buffer; the staging buffer is destroyed right after upload.
    const VkImageCreateInfo image_info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .extent = {AtlasWidth, AtlasHeight, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo image_alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(allocator, &image_info, &image_alloc, &hud_atlas_image,
                       &hud_atlas_allocation, nullptr) != VK_SUCCESS) {
        std::puts("HUD: visual overlay skipped (atlas image allocation failed)");
        hud_atlas_image = {};
        hud_atlas_allocation = {};
        return false;
    }

    hud_atlas_view = HudCheck(
        device.createImageView({
            .image = vk::Image{hud_atlas_image},
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8Unorm,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }),
        "atlas image view");

    const VkBufferCreateInfo staging_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = AtlasBytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo staging_alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer staging_buffer{};
    VmaAllocation staging_allocation{};
    VmaAllocationInfo staging_mapped{};
    if (vmaCreateBuffer(allocator, &staging_info, &staging_alloc, &staging_buffer,
                        &staging_allocation, &staging_mapped) != VK_SUCCESS ||
        !staging_mapped.pMappedData) {
        std::puts("HUD: visual overlay skipped (atlas staging allocation failed)");
        if (staging_buffer) {
            vmaDestroyBuffer(allocator, staging_buffer, staging_allocation);
        }
        return false;
    }
    std::memset(staging_mapped.pMappedData, 0, AtlasBytes);
    BakeAtlas(static_cast<u8*>(staging_mapped.pMappedData));
    vmaFlushAllocation(allocator, staging_allocation, 0, VK_WHOLE_SIZE);
    const bool uploaded = UploadAtlas(device, instance.GetGraphicsQueue(),
                                      instance.GetGraphicsQueueFamilyIndex(), staging_buffer);
    vmaDestroyBuffer(allocator, staging_buffer, staging_allocation);
    if (!uploaded || !hud_atlas_view) {
        std::puts("HUD: visual overlay skipped (atlas upload failed)");
        return false;
    }

    const vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
    };
    hud_sampler = HudCheck(device.createSampler(sampler_info), "sampler");
    if (!hud_sampler) {
        return false;
    }

    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eCombinedImageSampler, 1};
    hud_descriptor_pool = HudCheck(
        device.createDescriptorPool({
            .maxSets = 1,
            .poolSizeCount = 1,
            .pPoolSizes = &pool_size,
        }),
        "descriptor pool");
    if (!hud_descriptor_pool) {
        return false;
    }
    const vk::DescriptorSetLayout layouts[1]{hud_descriptor_set_layout};
    const auto descriptor_sets = HudCheck(
        device.allocateDescriptorSets({
            .descriptorPool = hud_descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = layouts,
        }),
        "descriptor set");
    if (descriptor_sets.empty()) {
        return false;
    }
    hud_descriptor_set = descriptor_sets.front();
    const vk::DescriptorImageInfo atlas_info{
        .sampler = hud_sampler,
        .imageView = hud_atlas_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const std::array set_writes{
        vk::WriteDescriptorSet{
            .dstSet = hud_descriptor_set,
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &atlas_info,
        },
    };
    device.updateDescriptorSets(set_writes, {});

    // One host-visible, permanently mapped vertex buffer per presentation slot.
    hud_slot_count = std::clamp(image_count, 1u, MaxImageSlots);
    const VkBufferCreateInfo vertex_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = MaxVertices * sizeof(HudVertex),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo vertex_alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    for (u32 i = 0; i < hud_slot_count; ++i) {
        VmaAllocationInfo mapped{};
        if (vmaCreateBuffer(allocator, &vertex_info, &vertex_alloc, &hud_slots[i].buffer,
                            &hud_slots[i].allocation, &mapped) != VK_SUCCESS ||
            !mapped.pMappedData) {
            std::printf("HUD: visual overlay skipped (vertex buffer %u allocation failed)\n", i);
            return false;
        }
        hud_slots[i].mapped = static_cast<HudVertex*>(mapped.pMappedData);
    }

    hud_ready = true;
    std::printf("HUD: UWP visual overlay ready (atlas %ux%u R8, %u slots, %u KiB)\n", AtlasWidth,
                AtlasHeight, hud_slot_count,
                (AtlasBytes + hud_slot_count * MaxVertices * u32(sizeof(HudVertex))) / 1024);
    return true;
}

/// Presentation slot for `view`; the swapchain image view identifies its image.
u32 SlotForView(vk::ImageView view) {
    const VkImageView raw = static_cast<VkImageView>(view);
    for (u32 i = 0; i < hud_used_slots; ++i) {
        if (hud_slots[i].view == raw) {
            return i;
        }
    }
    if (hud_used_slots < hud_slot_count) {
        hud_slots[hud_used_slots].view = raw;
        return hud_used_slots++;
    }
    const u32 slot = hud_round_robin++ % hud_slot_count;
    hud_slots[slot].view = raw;
    return slot;
}

/// Keeps a formatted line inside its 32-cell budget. `EmitText` stops at the total vertex
/// capacity, so an overlong line would silently push the next one out of the buffer.
void ClampLine(char* line, size_t size) {
    if (size == 0) {
        return;
    }
    const size_t limit = std::min<size_t>(MaxCharsPerLine, size - 1);
    if (std::strlen(line) > limit) {
        line[limit] = '\0';
    }
}

/// Appends the quads of `text` to `out` (capacity `MaxVertices`), returning the vertex count.
u32 EmitText(const char* text, float pen_x, float baseline, const float color[4],
             vk::Extent2D extent, HudVertex* out, u32 count) {
    const float half_width = static_cast<float>(extent.width) * 0.5f;
    const float half_height = static_cast<float>(extent.height) * 0.5f;
    const auto ndc_x = [half_width](float x) { return x / half_width - 1.0f; };
    // Vulkan framebuffer coordinates start at the top-left: NDC y = -1 is the top edge.
    const auto ndc_y = [half_height](float y) { return y / half_height - 1.0f; };
    float pen = pen_x;
    for (u32 i = 0; text[i] != '\0' && count + VerticesPerGlyph <= MaxVertices; ++i) {
        const int cp = static_cast<unsigned char>(text[i]);
        if (cp < 32 || cp > 126) {
            continue;
        }
        const HudGlyph& glyph = hud_glyphs[cp - 32];
        if (glyph.width > 0 && glyph.height > 0) {
            const u32 cell_x = (glyph.cell % AtlasColumns) * CellWidth;
            const u32 cell_y = (glyph.cell / AtlasColumns) * CellHeight;
            const float x0 = pen + static_cast<float>(glyph.xoff);
            const float y0 = baseline + static_cast<float>(glyph.yoff);
            const float x1 = x0 + static_cast<float>(glyph.width);
            const float y1 = y0 + static_cast<float>(glyph.height);
            const float u0 = static_cast<float>(cell_x + PadX + glyph.xoff) / AtlasWidth;
            const float v0 = static_cast<float>(cell_y + Baseline + glyph.yoff) / AtlasHeight;
            const float u1 = u0 + static_cast<float>(glyph.width) / AtlasWidth;
            const float v1 = v0 + static_cast<float>(glyph.height) / AtlasHeight;
            const float ax0 = ndc_x(x0), ax1 = ndc_x(x1);
            const float ay0 = ndc_y(y0), ay1 = ndc_y(y1);
            const float r = color[0], g = color[1], b = color[2], a = color[3];
            const HudVertex quad[VerticesPerGlyph]{
                {ax0, ay0, u0, v0, r, g, b, a}, {ax1, ay0, u1, v0, r, g, b, a},
                {ax1, ay1, u1, v1, r, g, b, a}, {ax0, ay0, u0, v0, r, g, b, a},
                {ax1, ay1, u1, v1, r, g, b, a}, {ax0, ay1, u0, v1, r, g, b, a},
            };
            std::memcpy(out + count, quad, sizeof(quad));
            count += VerticesPerGlyph;
        }
        pen += glyph.advance;
    }
    return count;
}
} // namespace

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    if (const char* env = std::getenv("BB_HUD"); env && env[0] == '0') {
        hud_enabled = false;
    }
    last_report = Clock::time_point{};
    std::puts("HUD: UWP RAM/FPS telemetry initialized (BB_HUD=0 disables)");
    std::fflush(stdout);
    if (!hud_enabled) {
        std::puts("HUD: visual overlay disabled by BB_HUD=0");
        return;
    }
    try {
        CreateResources(instance, format, image_count);
    } catch (...) {
        hud_ready = false;
        std::puts("HUD: visual overlay skipped (resources could not be created)");
    }
}

bool HandleEvent(const SDL_Event&) {
    return false;
}

void UpdateTextInput(SDL_Window*) {}

bool Visible() {
    return hud_enabled && hud_ready;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    if (!hud_ready || extent.width == 0 || extent.height == 0) {
        return;
    }
    const u32 slot = SlotForView(view);
    HudVertex* vertices = hud_slots[slot].mapped;
    if (!vertices || !hud_slots[slot].buffer) {
        return;
    }

    char ram_line[96];
    char rate_line[96];
    char trace_line[96];
    char quad_line[96];
    std::snprintf(ram_line, sizeof(ram_line), "RAM %llu/%llu/%llu MiB",
                  static_cast<unsigned long long>(memory_current_mib),
                  static_cast<unsigned long long>(memory_peak_mib),
                  static_cast<unsigned long long>(memory_limit_mib));
    const double fps = frame_ms_avg > 0.0 ? 1000.0 / frame_ms_avg : 0.0;
    std::snprintf(rate_line, sizeof(rate_line), "%.1f FPS %.2f ms", fps, frame_ms_avg);
    // GOAL.md phase 1: the three suspected costs of a gameplay frame, measured per present.
    // Integer milliseconds keep the line inside its 32-cell budget; a frame is 250-330 ms today.
    std::snprintf(trace_line, sizeof(trace_line), "FIN %llu %llums ARENA %llu",
                  static_cast<unsigned long long>(frame_counters.scheduler_finishes),
                  static_cast<unsigned long long>(frame_counters.scheduler_finish_us / 1000),
                  static_cast<unsigned long long>(frame_counters.arena_replacements));
    std::snprintf(quad_line, sizeof(quad_line), "QUAD %llu idx %llu R %llu",
                  static_cast<unsigned long long>(frame_counters.quad_draws),
                  static_cast<unsigned long long>(frame_counters.quad_indices),
                  static_cast<unsigned long long>(frame_counters.rect_draws));
    ClampLine(ram_line, sizeof(ram_line));
    ClampLine(rate_line, sizeof(rate_line));
    ClampLine(trace_line, sizeof(trace_line));
    ClampLine(quad_line, sizeof(quad_line));

    constexpr float ram_color[4]{1.0f, 1.0f, 1.0f, 1.0f};
    constexpr float rate_color[4]{0.70f, 1.0f, 0.75f, 1.0f};
    constexpr float trace_color[4]{1.0f, 0.85f, 0.55f, 1.0f};
    constexpr float quad_color[4]{0.70f, 0.85f, 1.0f, 1.0f};
    const float first_baseline = MarginPx + static_cast<float>(Baseline);
    u32 count = EmitText(ram_line, MarginPx, first_baseline, ram_color, extent, vertices, 0);
    count = EmitText(rate_line, MarginPx, first_baseline + LineSpacingPx, rate_color, extent,
                     vertices, count);
    count = EmitText(trace_line, MarginPx, first_baseline + LineSpacingPx * 2.0f, trace_color,
                     extent, vertices, count);
    count = EmitText(quad_line, MarginPx, first_baseline + LineSpacingPx * 3.0f, quad_color,
                     extent, vertices, count);
    if (count == 0) {
        return;
    }
    vmaFlushAllocation(hud_allocator, hud_slots[slot].allocation, 0, VK_WHOLE_SIZE);

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info{
        .renderArea{
            .extent{
                .width = extent.width,
                .height = extent.height,
            },
        },
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    };

    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, hud_pipeline);
    cmdbuf.setViewport(0, std::array{vk::Viewport{
                               .x = 0.0f,
                               .y = 0.0f,
                               .width = static_cast<float>(extent.width),
                               .height = static_cast<float>(extent.height),
                               .minDepth = 0.0f,
                               .maxDepth = 1.0f,
                           }});
    cmdbuf.setScissor(0, std::array{vk::Rect2D{
                              .offset = {0, 0},
                              .extent = {extent.width, extent.height},
                          }});
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, hud_pipeline_layout, 0,
                              std::array{hud_descriptor_set}, {});
    cmdbuf.bindVertexBuffers(0, std::array{vk::Buffer{hud_slots[slot].buffer}},
                             std::array<vk::DeviceSize, 1>{0});
    cmdbuf.beginRendering(rendering_info);
    cmdbuf.draw(count, 1, 0, 0);
    cmdbuf.endRendering();
}

void NotePresent() {
    if (!hud_enabled) {
        return;
    }
    const auto now = Clock::now();
    if (last_present != Clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(now - last_present).count();
        frame_ms_avg = frame_ms_avg == 0.0 ? ms : frame_ms_avg * 0.95 + ms * 0.05;
    }
    last_present = now;
    ++present_count;
    frame_counters = BbPort::Counters::TakeFrame();
    // The 5120 MiB ceiling is enforced per allocation, so the peak must not be missed between
    // log lines. Read AppMemoryUsage every present (a cheap property read) and keep the running
    // maximum; only formatting/printing stays limited to twice per second.
    try {
        memory_current_mib = winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20;
        memory_peak_mib = std::max(memory_peak_mib, memory_current_mib);
    } catch (...) {
        return;
    }
    if (now - last_report < std::chrono::milliseconds(500)) {
        return;
    }
    last_report = now;
    try {
        memory_limit_mib = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20;
        const double fps = frame_ms_avg > 0.0 ? 1000.0 / frame_ms_avg : 0.0;
        std::printf("HUD: %.1f FPS %.2f ms RAM %llu/%llu/%llu MiB frames=%llu "
                    "fin=%llu fin_ms=%.1f arena=%llu quad=%llu qidx=%llu rect=%llu\n",
                    fps, frame_ms_avg, static_cast<unsigned long long>(memory_current_mib),
                    static_cast<unsigned long long>(memory_peak_mib),
                    static_cast<unsigned long long>(memory_limit_mib),
                    static_cast<unsigned long long>(present_count),
                    static_cast<unsigned long long>(frame_counters.scheduler_finishes),
                    static_cast<double>(frame_counters.scheduler_finish_us) / 1000.0,
                    static_cast<unsigned long long>(frame_counters.arena_replacements),
                    static_cast<unsigned long long>(frame_counters.quad_draws),
                    static_cast<unsigned long long>(frame_counters.quad_indices),
                    static_cast<unsigned long long>(frame_counters.rect_draws));
        std::fflush(stdout);
    } catch (...) {
        // Telemetry must never take the renderer down.
    }
}

void NotifyTrophy(int grade, std::string game, std::string name, std::vector<unsigned char>) {
    std::printf("BbOverlay: trophy notice (grade %d, %s / %s) not drawn on UWP\n", grade,
                game.c_str(), name.c_str());
}

bool CapturesInput() {
    return false;
}

void SetTextEntry(bool, const std::string&, const std::string&) {}

} // namespace BbOverlay
#endif
