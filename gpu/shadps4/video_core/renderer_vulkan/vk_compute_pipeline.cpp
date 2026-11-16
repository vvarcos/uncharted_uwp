// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef UNCHARTED_UWP
// Raw marker channel: the assert text alone arrives as a bare file:line on the console.
extern "C" void bb_log_raw(const char* text);
#endif

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata, bool preloading /*=false*/)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    const auto debug_str = GetDebugString();
#ifdef UNCHARTED_UWP
    /* bbport: the ICD refuses compute pipelines with a generic -1, so the inputs that decide it are
     * logged: the LDS scratch (D3D12 FL 11_0 caps a group at 32 KiB while the guest may ask for
     * more), the resource counts, and the device's own limits. */
    {
        const auto limits = instance.GetPhysicalDevice().getProperties().limits;
        char line[352];
        std::snprintf(line, sizeof(line),
                      "M5 cpso: %s lds=%u bindings{ssbo=%zu img=%zu smp=%zu} "
                      "limits{lds=%u invocations=%u size=%u,%u,%u} subgroup=%u",
                      debug_str.c_str(), info->shared_memory_scratch_size, info->buffers.size(),
                      info->images.size(), info->samplers.size(), limits.maxComputeSharedMemorySize,
                      limits.maxComputeWorkGroupInvocations, limits.maxComputeWorkGroupSize[0],
                      limits.maxComputeWorkGroupSize[1], limits.maxComputeWorkGroupSize[2],
                      instance.FixedComputeSubgroupSize());
        bb_log_raw(line);
        {
            /* bbport: per image, the descriptor kind and what the guest declares: a storage image in
             * a shader that still creates rules out "no storage images in compute", and is_integer
             * decides whether sampling it is legal on this device at all. */
            char images[384]{};
            size_t used = 0;
            u32 index = 0;
            for (const auto& image : info->images) {
                const auto img_sharp = image.GetSharp(*info);
                used += std::snprintf(
                    images + used, sizeof(images) - used,
                    "%si%u{w=%d,n=%u,fmt=%d,int=%d,cube=%d,arr=%d,dim=%d}", used ? " " : "",
                    index++, int(image.is_written), unsigned(image.NumBindings(*info)),
                    img_sharp ? int(img_sharp.GetNumberFmt()) : -1,
                    img_sharp ? int(AmdGpu::IsInteger(img_sharp.GetNumberFmt())) : -1,
                    img_sharp ? int(img_sharp.IsCube()) : -1, int(image.is_array),
                    img_sharp ? int(img_sharp.GetViewType(image.is_array)) : -1);
                if (used + 64 >= sizeof(images)) {
                    break;
                }
            }
            std::snprintf(line, sizeof(line), "M5 cso2: %s img[%s]", debug_str.c_str(), images);
            bb_log_raw(line);
        }
    }
#endif
    // 64 lanes where the device has them, else the device's own size where the driver could pick another one (Intel).
    const u32 subgroup_size =
        instance.IsSubgroupSize64Supported() ? 64 : instance.FixedComputeSubgroupSize();
    const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
        .requiredSubgroupSize = subgroup_size,
    };
    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .pNext = subgroup_size ? &subgroup_size_ci : nullptr,
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
    };

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
#ifdef UNCHARTED_UWP
    /* bbport: BB_PAD_BINDINGS=N appends N unused sampled-image bindings to this layout. Vulkan allows
     * a layout to declare bindings the shader does not use, so padding changes only the layout shape.
     * That isolates "the count/shape of a compute layout breaks creation" from "this particular shader
     * breaks creation" without writing a synthetic shader. */
    if (const char* pad = std::getenv("BB_PAD_BINDINGS")) {
        const int extra = std::atoi(pad);
        for (int i = 0; i < extra; ++i) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampledImage,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute,
            });
        }
        char line[128];
        std::snprintf(line, sizeof(line), "M5 cso: padded +%d bindings, total=%u", extra,
                      unsigned(bindings.size()));
        bb_log_raw(line);
    }
#endif
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pipeline_layout,
    };
    auto [pipeline_result, pipe] =
        instance.GetDevice().createComputePipelineUnique(pipeline_cache, compute_pipeline_ci);
#ifdef UNCHARTED_UWP
    {
        char line[256];
        std::snprintf(line, sizeof(line), "M5 pipe: call=computepso result=%d (%s) %s",
                      int(pipeline_result), vk::to_string(pipeline_result).c_str(),
                      debug_str.c_str());
        bb_log_raw(line);
    }
#endif
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Compute Pipeline {}", debug_str);
}

ComputePipeline::~ComputePipeline() = default;

} // namespace Vulkan
