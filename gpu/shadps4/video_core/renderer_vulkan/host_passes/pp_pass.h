//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include <vector>

namespace Vulkan {
class Frame;
}

namespace Vulkan::HostPasses {

class PostProcessingPass {
public:
    struct Settings {
        float gamma = 1.0f;
        u32 hdr = 0;
        u32 srgb_input = 0;
    };

    void Create(vk::Device device, vk::Format surface_format, u32 num_frames);

    // Each frame has two distinct slots: normal and optional HUD-less draw in the same command buffer.
    // The present_done fence must signal before reusing that frame's slots.
    void Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                Frame& output, Settings settings, u32 descriptor_slot);

private:
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniqueSampler sampler{};
    vk::Device descriptor_device{};
    vk::UniqueDescriptorPool descriptor_pool{};
    std::vector<vk::DescriptorSet> descriptor_sets{};
};

} // namespace Vulkan::HostPasses
