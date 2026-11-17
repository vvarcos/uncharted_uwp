// SPDX-License-Identifier: GPL-2.0-or-later
// EXPERIMENTAL opt-in backend: standalone R32 <-> D32 depth copy for the UWP port.
// Nothing in the runtime references this class; the root must opt in explicitly.
#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

/// Records a full-screen triangle that reads the .r channel of a sampled source image and
/// writes it either to a depth-only attachment (gl_FragDepth) or to an R32 color attachment.
///
/// The caller owns all image layouts and barriers: before Copy(), src must be in
/// eShaderReadOnlyOptimal and dst in eDepthStencilAttachmentOptimal / eColorAttachmentOptimal.
/// Only single-sample 2D images, mip 0 / layer 0 are supported.
class UwpDepthCopy {
public:
    UwpDepthCopy(const Instance& instance, Scheduler& scheduler);
    ~UwpDepthCopy();

    UwpDepthCopy(const UwpDepthCopy&) = delete;
    UwpDepthCopy& operator=(const UwpDepthCopy&) = delete;
    UwpDepthCopy(UwpDepthCopy&&) = delete;
    UwpDepthCopy& operator=(UwpDepthCopy&&) = delete;

    /// Returns false (and records nothing) on unsupported formats or any creation failure.
    bool Copy(vk::CommandBuffer cmdbuf, vk::Image src, vk::Image dst, vk::Format src_format,
              vk::Format dst_format, vk::Extent2D extent);

    /// False when shader/layout/pool creation failed; Copy() then always returns false.
    bool IsValid() const noexcept {
        return valid_;
    }

private:
    enum class Target { Depth, Color };

    struct PipelineEntry {
        vk::Format format{};
        vk::Pipeline pipeline{};
    };

    bool CreateShaders();
    bool CreateLayoutsAndPool();
    bool EnsurePipeline(Target target, vk::Format format, vk::Pipeline& out);
    bool CreatePipeline(Target target, vk::Format format, vk::Pipeline& out);
    static bool IsSupported(vk::Format format) noexcept;

    const Instance& instance;
    Scheduler& scheduler;

    vk::ShaderModule fs_tri_vertex{};
    vk::ShaderModule copy_frag{};
    vk::DescriptorSetLayout descriptor_set_layout{};
    vk::PipelineLayout pipeline_layout{};
    struct PoolLifetime {
        vk::Device device;
        vk::DescriptorPool pool;
        ~PoolLifetime() { device.destroyDescriptorPool(pool); }
    };
    vk::DescriptorPool descriptor_pool{};
    std::shared_ptr<PoolLifetime> pool_lifetime;

    // At most two pipelines: keyed by destination format (depth vs color).
    std::array<PipelineEntry, 2> pipelines{};
    std::size_t pipeline_count = 0;
    bool valid_ = false;
};

} // namespace Vulkan
