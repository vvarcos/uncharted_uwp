// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"

#include <atomic>
#include <cstdio>
#include <magic_enum/magic_enum.hpp>
#include <vk_mem_alloc.h>

namespace VideoCore {

// Worst case tiling dispatches the pool has to hold before the scheduler drains and resets it.
constexpr u32 kMaxTilingSets = 512;

struct TilingInfo {
    u32 bank_swizzle;
    u32 num_slices;
    u32 num_mips;
    std::array<ImageInfo::MipInfo, 16> mips;
};

TileManager::TileManager(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, StreamBuffer& stream_buffer_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, stream_buffer{stream_buffer_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 2,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};

    push_supported = instance.MaxPushDescriptors() > 0;
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = push_supported ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                                : vk::DescriptorSetLayoutCreateFlagBits{},
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto desc_layout_result = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(desc_layout_result.result == vk::Result::eSuccess,
               "Failed to create descriptor set layout: {}",
               vk::to_string(desc_layout_result.result));
    desc_layout = std::move(desc_layout_result.value);

    if (!push_supported) {
        std::printf("M5 tiling: no push descriptors, using descriptor pools\n");
        std::fflush(stdout);
        AddDescriptorPool();
    }

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 0U,
        .pPushConstantRanges = nullptr,
    };
    auto [layout_result, layout] = device.createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(layout);
}

TileManager::~TileManager() = default;

void TileManager::AddDescriptorPool() {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorPoolSize, 2> pool_sizes = {{
        {vk::DescriptorType::eStorageBuffer, 2 * kMaxTilingSets},
        {vk::DescriptorType::eUniformBuffer, kMaxTilingSets},
    }};
    const vk::DescriptorPoolCreateInfo pool_ci = {
        .maxSets = kMaxTilingSets,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };
    desc_pools.push_back(::Vulkan::Check(device.createDescriptorPoolUnique(pool_ci)));
}

vk::Pipeline TileManager::GetTilingPipeline(const ImageInfo& info, bool is_tiler) {
    const TilingKey key{
        .tile_mode = info.tile_mode,
        .num_bits = info.num_bits,
        .num_samples = info.num_samples,
        .is_tiler = is_tiler,
    };
    if (const auto it = tiling_pipelines.find(key); it != tiling_pipelines.end()) {
        return *it->second;
    }

    const auto device = instance.GetDevice();
    const auto micro_tile_mode = AmdGpu::GetMicroTileMode(info.tile_mode);
    const bool is_macro = AmdGpu::IsMacroTiled(info.array_mode);
    std::array<u32, 12> spec_data{
        info.num_samples,
        u32(micro_tile_mode),
        AmdGpu::GetMicroTileThickness(info.array_mode),
        u32(is_tiler),
    };
    if (is_macro) {
        const auto macro_tile_mode =
            AmdGpu::CalculateMacrotileMode(info.tile_mode, info.num_bits, info.num_samples);
        spec_data[4] = u32(info.array_mode);
        spec_data[5] = u32(AmdGpu::GetPipeConfig(info.tile_mode));
        spec_data[6] = AmdGpu::GetBankWidth(macro_tile_mode);
        spec_data[7] = AmdGpu::GetBankHeight(macro_tile_mode);
        spec_data[8] = AmdGpu::GetNumBanks(macro_tile_mode);
        spec_data[9] = std::bit_width(spec_data[8]) - 1;
        spec_data[10] = AmdGpu::CalculateTileSplit(info.tile_mode, info.array_mode, micro_tile_mode,
                                                   info.num_bits);
        spec_data[11] = AmdGpu::GetMacrotileAspect(macro_tile_mode);
    }

    std::span<const u32> code;
    switch (info.num_bits) {
    case 8:
        code = is_macro ? std::span<const u32>{TILING_MACRO_8_COMP}
                        : std::span<const u32>{TILING_MICRO_8_COMP};
        break;
    case 16:
        code = is_macro ? std::span<const u32>{TILING_MACRO_16_COMP}
                        : std::span<const u32>{TILING_MICRO_16_COMP};
        break;
    case 32:
        code = is_macro ? std::span<const u32>{TILING_MACRO_32_COMP}
                        : std::span<const u32>{TILING_MICRO_32_COMP};
        break;
    case 64:
        code = is_macro ? std::span<const u32>{TILING_MACRO_64_COMP}
                        : std::span<const u32>{TILING_MICRO_64_COMP};
        break;
    case 96:
        code = is_macro ? std::span<const u32>{TILING_MACRO_96_COMP}
                        : std::span<const u32>{TILING_MICRO_96_COMP};
        break;
    case 128:
        code = is_macro ? std::span<const u32>{TILING_MACRO_128_COMP}
                        : std::span<const u32>{TILING_MICRO_128_COMP};
        break;
    default:
        UNREACHABLE_MSG("Unsupported tiling pixel width {}", info.num_bits);
    }

    const auto module = Vulkan::CompileSPV(code, device);
    static constexpr auto spec_entries = [] {
        std::array<vk::SpecializationMapEntry, 12> entries{};
        for (u32 i = 0; i < entries.size(); ++i) {
            entries[i] = vk::SpecializationMapEntry{i, i * u32(sizeof(u32)), sizeof(u32)};
        }
        return entries;
    }();

    const u32 spec_count = is_macro ? 12 : 4;
    const vk::SpecializationInfo specialization{
        .mapEntryCount = spec_count,
        .pMapEntries = spec_entries.data(),
        .dataSize = spec_count * sizeof(u32),
        .pData = spec_data.data(),
    };

    const auto module_name = fmt::format("{}_{} {}", magic_enum::enum_name(info.tile_mode),
                                         info.num_bits, is_tiler ? "tiler" : "detiler");
    LOG_INFO(Render_Vulkan, "Creating tiling pipeline {}", module_name);
    Vulkan::SetObjectName(device, module, module_name);

    const vk::PipelineShaderStageCreateInfo shader_ci = {
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = module,
        .pName = "main",
        .pSpecializationInfo = &specialization,
    };
    const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
        .stage = shader_ci,
        .layout = *pl_layout,
    };

    auto [result, pipeline] =
        device.createComputePipelineUnique(VK_NULL_HANDLE, compute_pipeline_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "Detiler pipeline creation failed {}",
               vk::to_string(result));
    const auto handle = *pipeline;
    tiling_pipelines.emplace(key, std::move(pipeline));
    device.destroyShaderModule(module);
    return handle;
}

void TileManager::RecordTilingDispatch(vk::Pipeline pipeline, const vk::DescriptorBufferInfo& tiled,
                                       const vk::DescriptorBufferInfo& linear,
                                       const vk::DescriptorBufferInfo& params, u32 dim_x) {
    scheduler.Record([this, pipeline, layout = *pl_layout, tiled, linear, params,
                      dim_x](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        const std::array<vk::WriteDescriptorSet, 3> set_writes = {{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &tiled,
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &linear,
            },
            {
                .dstBinding = 2,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo = &params,
            },
        }};
        if (push_supported) {
            cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, set_writes);
        } else {
            /* bbport: this device has no push descriptors, and the ICD answers a push with a
             * trampoline into a null table entry (that is the "call to 0x0"). Bind a real set
             * instead. A set is used by exactly one dispatch: descriptors are read when the command
             * buffer runs, so reusing one set between two dispatches would make the first dispatch
             * read the second one's range. */
            const auto device = instance.GetDevice();
            const vk::DescriptorSetLayout set_layout = *desc_layout;
            vk::DescriptorSetAllocateInfo alloc_info = {
                .descriptorPool = *desc_pools.back(),
                .descriptorSetCount = 1,
                .pSetLayouts = &set_layout,
            };
            /* No exceptions in this build: the result carries the error. A full pool chains a new
             * one — never skip the dispatch, because the submission would stay incomplete and the
             * guest's gpu-idle poll would never see it finish. */
            auto allocated = device.allocateDescriptorSets(alloc_info);
            if (allocated.result != vk::Result::eSuccess) {
                AddDescriptorPool();
                alloc_info.descriptorPool = *desc_pools.back();
                allocated = device.allocateDescriptorSets(alloc_info);
            }
            if (allocated.result != vk::Result::eSuccess || allocated.value.empty()) {
                ++sets_skipped;
                std::printf("M5 tiling: descriptor allocate failed (%s) pools=%zu sets=%llu "
                            "skipped=%u; dispatch skipped\n",
                            vk::to_string(allocated.result).c_str(), desc_pools.size(),
                            static_cast<unsigned long long>(sets_total), sets_skipped);
                std::fflush(stdout);
                return;
            }
            const vk::DescriptorSet set = allocated.value.front();
            /* dstSet is ignored for a push-descriptor write, so the shared writes carry none. A
             * real vkUpdateDescriptorSets requires it, and the ICD reads the set immediately —
             * leaving it null faults inside the driver reading offset 0x1e0 of address 0. */
            auto writes = set_writes;
            for (auto& write : writes) {
                write.dstSet = set;
            }
            device.updateDescriptorSets(writes, {});
            cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, layout, 0, set, {});
            ++sets_in_use;
            ++sets_total;
            /* One line every 512 sets: how many pools the frame needed and whether anything was
             * skipped. A skipped dispatch would explain a gpu-idle poll that never returns. */
            if ((sets_total & 0x1FF) == 0) {
                std::printf("M5 tiling: sets=%llu pools=%zu skipped=%u\n",
                            static_cast<unsigned long long>(sets_total), desc_pools.size(),
                            sets_skipped);
                std::fflush(stdout);
            }
        }
        cmdbuf.dispatch(dim_x, 1, 1);
    });
}

std::pair<const Buffer*, u64> TileManager::DetileImage(const VideoCore::Buffer* in_buffer,
                                                       u64 in_offset, const ImageInfo& info) {
    // bbport: raw lines so a console log says whether the detile runs for the formats on screen. The
    // block (BC) shapes are the ones that matter for the magenta scene, so they are never skipped by
    // the report cap.
    {
        static std::atomic<u32> block_reports{0};
        static std::atomic<u32> other_reports{0};
        const bool report = info.props.is_block
                                ? block_reports.fetch_add(1, std::memory_order_relaxed) < 16
                                : other_reports.fetch_add(1, std::memory_order_relaxed) < 8;
        if (report) {
            const auto& m0 = info.mips_layout[0];
            std::printf("M5 detile: tiled=%d tile_mode=%u bits=%u block=%d %ux%u levels=%u "
                        "mip0 pitch=%u height=%u size=%u\n",
                        int(info.props.is_tiled), u32(info.tile_mode), u32(info.num_bits),
                        int(info.props.is_block), info.size.width, info.size.height,
                        u32(info.resources.levels), m0.pitch, m0.height, m0.size);
            std::fflush(stdout);
        }
    }
    if (!info.props.is_tiled) {
        return {in_buffer, in_offset};
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = info.resources.levels;
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);

    scheduler.EndRendering();
    runtime.FlushBarriers();

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = in_buffer->Handle(),
        .offset = in_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
        .range = info.guest_size,
    };
    const auto dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    // bbport: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(GetTilingPipeline(info, false), tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, dim_x);

    runtime.AccessBuffer(staging.buffer, staging.offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);

    return {staging.buffer, staging.offset};
}

void TileManager::TileImage(Image& in_image, std::span<vk::BufferImageCopy> buffer_copies,
                            const VideoCore::Buffer* out_buffer, u64 out_offset) {
    const auto& info = in_image.info;
    if (!info.props.is_tiled) {
        for (auto& copy : buffer_copies) {
            copy.bufferOffset += out_offset;
        }
        runtime.DownloadImage(&in_image, out_buffer, buffer_copies);
        return;
    }

    TilingInfo params{};
    params.bank_swizzle = info.bank_swizzle;
    params.num_slices = info.props.is_volume ? info.size.depth : info.resources.layers;
    params.num_mips = static_cast<u32>(buffer_copies.size());
    for (u32 mip = 0; mip < params.num_mips; ++mip) {
        auto& mip_info = params.mips[mip];
        mip_info = info.mips_layout[mip];
        if (info.props.is_block) {
            mip_info.pitch = std::max((mip_info.pitch + 3) / 4, 1U);
            mip_info.height = std::max((mip_info.height + 3) / 4, 1U);
        }
    }

    const vk::DescriptorBufferInfo params_buffer_info{
        .buffer = stream_buffer.Handle(),
        .offset = stream_buffer.Copy(&params, sizeof(params), instance.UniformMinAlignment()),
        .range = sizeof(params),
    };

    const auto staging = runtime.GetStagingPool().Request(info.guest_size, MemoryType::DeviceLocal);
    for (auto& copy : buffer_copies) {
        copy.bufferOffset += staging.offset;
    }

    runtime.DownloadImage(&in_image, staging.buffer, buffer_copies);
    runtime.FlushBarriers();

    const vk::DescriptorBufferInfo tiled_buffer_info{
        .buffer = out_buffer->Handle(),
        .offset = out_offset,
        .range = info.guest_size,
    };
    const vk::DescriptorBufferInfo linear_buffer_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
        .range = info.guest_size,
    };
    const auto dim_x = static_cast<u32>((info.guest_size / (info.num_bits / 8)) / 64);
    // bbport: recorded with copies of the descriptor infos (threaded recording).
    RecordTilingDispatch(GetTilingPipeline(info, true), tiled_buffer_info, linear_buffer_info,
                         params_buffer_info, dim_x);

    runtime.AccessBuffer(out_buffer, out_offset, info.guest_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);
}

} // namespace VideoCore
