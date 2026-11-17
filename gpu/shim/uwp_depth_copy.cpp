// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef UNCHARTED_UWP
#include "uwp_depth_copy.h"

#include <array>
#include <utility>

#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/host_shaders/uwp_depth_copy_frag.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

UwpDepthCopy::UwpDepthCopy(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    // Short-circuit: no point building layouts when a shader failed. The destructor still
    // releases whatever was already created.
    valid_ = CreateShaders() && CreateLayoutsAndPool();
}

UwpDepthCopy::~UwpDepthCopy() {
    // Keep the pool alive until its deferred frees and the last GPU use have retired.
    scheduler.DeferOperation([device = instance.GetDevice(), entries = pipelines,
                              count = pipeline_count, layout = pipeline_layout,
                              set_layout = descriptor_set_layout, vertex = fs_tri_vertex,
                              fragment = copy_frag, pool = pool_lifetime] {
        for (std::size_t i = 0; i < count; ++i) {
            device.destroyPipeline(entries[i].pipeline);
        }
        device.destroyPipelineLayout(layout);
        device.destroyDescriptorSetLayout(set_layout);
        device.destroyShaderModule(vertex);
        device.destroyShaderModule(fragment);
    });
}

bool UwpDepthCopy::IsSupported(vk::Format format) noexcept {
    // VK_FORMAT_R32_SFLOAT = 100, VK_FORMAT_D32_SFLOAT_S8_UINT = 130.
    return format == vk::Format::eR32Sfloat || format == vk::Format::eD32SfloatS8Uint;
}

bool UwpDepthCopy::CreateShaders() {
    const auto device = instance.GetDevice();
    fs_tri_vertex = Vulkan::CompileSPV(FS_TRI_VERT, device);
    copy_frag = Vulkan::CompileSPV(UWP_DEPTH_COPY_FRAG, device);
    return static_cast<bool>(fs_tri_vertex) && static_cast<bool>(copy_frag);
}

bool UwpDepthCopy::CreateLayoutsAndPool() {
    const auto device = instance.GetDevice();

    // Ordinary sampled image (no sampler): texelFetch ignores sampler state. No push
    // descriptors, no descriptor indexing, no robustness, no BDA.
    const vk::DescriptorSetLayoutBinding binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    auto [layout_result, layout] = device.createDescriptorSetLayout({
        .bindingCount = 1,
        .pBindings = &binding,
    });
    if (layout_result != vk::Result::eSuccess) {
        return false;
    }
    descriptor_set_layout = layout;

    auto [layout_create_result, created_pipeline_layout] = device.createPipelineLayout({
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
    });
    if (layout_create_result != vk::Result::eSuccess) {
        return false;
    }
    pipeline_layout = created_pipeline_layout;

    // One set per Copy(); eFreeDescriptorSet lets each set be freed (not reset) after the
    // GPU has consumed it. 256 in-flight copies is the hard cap.
    const vk::DescriptorPoolSize pool_size{vk::DescriptorType::eSampledImage, 256};
    auto [pool_result, pool] = device.createDescriptorPool({
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets = 256,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    if (pool_result != vk::Result::eSuccess) {
        return false;
    }
    descriptor_pool = pool;
    pool_lifetime = std::make_shared<PoolLifetime>();
    pool_lifetime->device = device;
    pool_lifetime->pool = pool;
    return true;
}

bool UwpDepthCopy::CreatePipeline(Target target, vk::Format format, vk::Pipeline& out) {
    const auto device = instance.GetDevice();

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineDepthStencilStateCreateInfo depth_state{
        .depthTestEnable = target == Target::Depth,
        .depthWriteEnable = target == Target::Depth,
        .depthCompareOp = vk::CompareOp::eAlways,
        .stencilTestEnable = false,
    };
    // Normal (non-withCount) dynamic viewport/scissor, as in the UWP HUD path.
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const std::array shader_stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = fs_tri_vertex,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = copy_frag,
            .pName = "main",
        },
    };

    vk::Format color_format = vk::Format::eUndefined;
    vk::Format depth_format = vk::Format::eUndefined;
    if (target == Target::Depth) {
        depth_format = format;
    } else {
        color_format = format;
    }
    const vk::PipelineRenderingCreateInfo rendering_ci{
        .colorAttachmentCount = target == Target::Color ? 1u : 0u,
        .pColorAttachmentFormats = target == Target::Color ? &color_format : nullptr,
        .depthAttachmentFormat = depth_format,
        .stencilAttachmentFormat = vk::Format::eUndefined,
    };

    const vk::PipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .attachmentCount = target == Target::Color ? 1u : 0u,
        .pAttachments = target == Target::Color ? &blend_attachment : nullptr,
    };
    const vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1,
        .scissorCount = 1,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineRasterizationStateCreateInfo raster_state{.lineWidth = 1.f};

    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &rendering_ci,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depth_state,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = pipeline_layout,
    };
    auto [pipeline_result, pipeline] = device.createGraphicsPipeline(VK_NULL_HANDLE, pipeline_info);
    if (pipeline_result != vk::Result::eSuccess) {
        return false;
    }
    out = pipeline;
    return true;
}

bool UwpDepthCopy::EnsurePipeline(Target target, vk::Format format, vk::Pipeline& out) {
    for (std::size_t i = 0; i < pipeline_count; ++i) {
        if (pipelines[i].format == format) {
            out = pipelines[i].pipeline;
            return true;
        }
    }
    vk::Pipeline pipeline{};
    if (!CreatePipeline(target, format, pipeline)) {
        return false;
    }
    if (pipeline_count >= pipelines.size()) {
        // Unreachable with the two supported formats, but never leak a rejected pipeline.
        instance.GetDevice().destroy(pipeline);
        return false;
    }
    pipelines[pipeline_count++] = PipelineEntry{format, pipeline};
    out = pipeline;
    return true;
}

bool UwpDepthCopy::Copy(vk::CommandBuffer cmdbuf, vk::Image src, vk::Image dst,
                        vk::Format src_format, vk::Format dst_format, vk::Extent2D extent) {
    if (!valid_ || !cmdbuf || !src || !dst || extent.width == 0 || extent.height == 0) {
        return false;
    }
    if (!IsSupported(src_format) || !IsSupported(dst_format)) {
        return false;
    }

    const Target target =
        dst_format == vk::Format::eD32SfloatS8Uint ? Target::Depth : Target::Color;
    const auto device = instance.GetDevice();

    // Source: sampled image, depth aspect for D32 / color aspect for R32, mip 0 / layer 0.
    const vk::ImageViewUsageCreateInfo src_usage_ci{.usage = vk::ImageUsageFlagBits::eSampled};
    const vk::ImageViewCreateInfo src_view_ci{
        .pNext = &src_usage_ci,
        .image = src,
        .viewType = vk::ImageViewType::e2D,
        .format = src_format,
        .subresourceRange{
            .aspectMask = src_format == vk::Format::eD32SfloatS8Uint
                              ? vk::ImageAspectFlagBits::eDepth
                              : vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto [src_view_result, src_view] = device.createImageView(src_view_ci);
    if (src_view_result != vk::Result::eSuccess) {
        return false;
    }

    // Destination: depth-only view for DS, color view for R32. Both views are transient and
    // are destroyed on the scheduler tick together with the descriptor set.
    const vk::ImageViewUsageCreateInfo dst_usage_ci{
        .usage = target == Target::Depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                                         : vk::ImageUsageFlagBits::eColorAttachment,
    };
    const vk::ImageViewCreateInfo dst_view_ci{
        .pNext = &dst_usage_ci,
        .image = dst,
        .viewType = vk::ImageViewType::e2D,
        .format = dst_format,
        .subresourceRange{
            .aspectMask = target == Target::Depth ? vk::ImageAspectFlagBits::eDepth
                                                  : vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto [dst_view_result, dst_view] = device.createImageView(dst_view_ci);
    if (dst_view_result != vk::Result::eSuccess) {
        device.destroyImageView(src_view);
        return false;
    }
    scheduler.DeferOperation([device, src_view, dst_view] {
        device.destroyImageView(src_view);
        device.destroyImageView(dst_view);
    });

    // Fresh set and fresh (stack-local) write per command: no shared mutable write state.
    const vk::DescriptorSetAllocateInfo alloc_info{
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout,
    };
    auto [alloc_result, sets] = device.allocateDescriptorSets(alloc_info);
    if (alloc_result != vk::Result::eSuccess || sets.empty()) {
        return false;
    }
    const vk::DescriptorSet set = sets.front();

    const vk::DescriptorImageInfo image_info{
        .sampler = VK_NULL_HANDLE,
        .imageView = src_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write{
        .dstSet = set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eSampledImage,
        .pImageInfo = &image_info,
    };
    device.updateDescriptorSets(1, &write, 0, nullptr);
    scheduler.DeferOperation([device, owner = pool_lifetime, set] {
        (void)device.freeDescriptorSets(owner->pool, 1, &set);
    });

    vk::Pipeline pipeline{};
    if (!EnsurePipeline(target, dst_format, pipeline)) {
        return false;
    }

    vk::RenderingAttachmentInfo color_attachment{};
    vk::RenderingAttachmentInfo depth_attachment{};
    vk::RenderingInfo rendering_info{};
    rendering_info.renderArea = vk::Rect2D{{0, 0}, extent};
    rendering_info.layerCount = 1;
    if (target == Target::Depth) {
        depth_attachment = vk::RenderingAttachmentInfo{
            .imageView = dst_view,
            .imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        rendering_info.pDepthAttachment = &depth_attachment;
    } else {
        color_attachment = vk::RenderingAttachmentInfo{
            .imageView = dst_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp = vk::AttachmentLoadOp::eDontCare,
            .storeOp = vk::AttachmentStoreOp::eStore,
        };
        rendering_info.colorAttachmentCount = 1;
        rendering_info.pColorAttachments = &color_attachment;
    }

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
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
                              .extent = extent,
                          }});
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                              std::array{set}, {});
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();
    // Raw viewport/scissor commands bypass the scheduler cache; mark it dirty like BlitHelper.
    scheduler.GetDynamicState().Invalidate();
    return true;
}

} // namespace Vulkan
#endif // UNCHARTED_UWP
