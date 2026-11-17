// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <span>
#ifdef UNCHARTED_UWP
#include <atomic>
#include <cstdio>
#endif

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;


void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([&](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, set_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : set_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(std::span<const vk::WriteDescriptorSet>{set_writes});
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
#ifdef UNCHARTED_UWP
    // The Continue crash enters Dozen from vkUpdateDescriptorSets. Without robustness2, null
    // buffer/image descriptors are not supported. Diagnose them here without silently dropping a
    // dispatch or substituting data; a valid-looking but destroyed handle needs lifetime tracing.
    // Keep separate budgets: menu draws must not exhaust the diagnostics before the first
    // compute dispatch reached by Continue.
    static std::atomic<u32> descriptor_warnings[2]{};
    for (const auto& write : set_writes) {
        for (u32 i = 0; i < write.descriptorCount; ++i) {
            const char* problem = nullptr;
            switch (write.descriptorType) {
            case vk::DescriptorType::eStorageBuffer:
            case vk::DescriptorType::eUniformBuffer:
            case vk::DescriptorType::eStorageBufferDynamic:
            case vk::DescriptorType::eUniformBufferDynamic:
                if (!write.pBufferInfo) {
                    problem = "missing-buffer-info";
                } else if (!write.pBufferInfo[i].buffer) {
                    problem = "null-buffer";
                } else if (write.pBufferInfo[i].range == 0) {
                    problem = "zero-buffer-range";
                }
                break;
            case vk::DescriptorType::eSampledImage:
            case vk::DescriptorType::eStorageImage:
            case vk::DescriptorType::eInputAttachment:
            case vk::DescriptorType::eCombinedImageSampler:
                if (!write.pImageInfo) {
                    problem = "missing-image-info";
                } else if (!write.pImageInfo[i].imageView) {
                    problem = "null-image-view";
                } else if (write.descriptorType == vk::DescriptorType::eCombinedImageSampler &&
                           !write.pImageInfo[i].sampler) {
                    problem = "null-combined-sampler";
                }
                break;
            case vk::DescriptorType::eSampler:
                if (!write.pImageInfo || !write.pImageInfo[i].sampler) {
                    problem = "null-sampler";
                }
                break;
            case vk::DescriptorType::eUniformTexelBuffer:
            case vk::DescriptorType::eStorageTexelBuffer:
                if (!write.pTexelBufferView || !write.pTexelBufferView[i]) {
                    problem = "null-buffer-view";
                }
                break;
            default:
                break;
            }
            if (problem && descriptor_warnings[IsCompute() ? 1 : 0].fetch_add(
                               1, std::memory_order_relaxed) < 64) {
                const bool buffer_type = write.descriptorType == vk::DescriptorType::eStorageBuffer ||
                                         write.descriptorType == vk::DescriptorType::eUniformBuffer ||
                                         write.descriptorType == vk::DescriptorType::eStorageBufferDynamic ||
                                         write.descriptorType == vk::DescriptorType::eUniformBufferDynamic;
                const auto* info = buffer_type && write.pBufferInfo ? &write.pBufferInfo[i] : nullptr;
                char line[256];
                std::snprintf(line, sizeof(line),
                              "M5 descriptor invalid: compute=%u binding=%u element=%u type=%u "
                              "count=%u issue=%s offset=%llu range=%llu\n",
                              unsigned(IsCompute()), write.dstBinding, write.dstArrayElement + i,
                              unsigned(write.descriptorType), write.descriptorCount, problem,
                              static_cast<unsigned long long>(info ? info->offset : 0),
                              static_cast<unsigned long long>(info ? info->range : 0));
                std::fputs(line, stdout);
                std::fflush(stdout);
            }
        }
    }
#endif
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
