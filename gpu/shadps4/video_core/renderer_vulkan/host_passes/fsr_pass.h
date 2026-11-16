//  SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"
#ifdef UNCHARTED_UWP
#include <vk_mem_alloc.h>
#endif

namespace Vulkan::HostPasses {

#ifdef UNCHARTED_UWP
/// UWP: the upscaler owns plain VMA images instead of VideoCore::UniqueImage, which drags the
/// whole texture cache/rasterizer into the console binary (and its load-time initializers).
struct UwpFsrImage {
    VkImage handle{};
    VmaAllocation allocation{};

    void Create(VmaAllocator allocator, const vk::ImageCreateInfo& create_info);
    void Destroy(VmaAllocator allocator);
    operator vk::Image() const {
        return vk::Image{handle};
    }
};
#endif

class FsrPass {
public:
    struct Settings {
        bool enable{true};
        bool use_rcas{true};
        float rcas_attenuation{0.25f};
    };

    void Create(vk::Device device, VmaAllocator allocator, u32 num_images);

    // UWP: `descriptor_slot` selects an immutable descriptor slot pair (EASU/RCAS); pass a
    // different slot for each draw of a frame because both may be in the same command buffer.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, vk::ImageView input, vk::Extent2D input_size,
                         vk::Extent2D output_size, Settings settings, bool hdr,
                         u32 descriptor_slot = 0);

private:
    struct Img {
        u8 id{};
        bool dirty{true};

#ifdef UNCHARTED_UWP
        UwpFsrImage intermediary_image;
        vk::UniqueImageView intermediary_image_view;

        UwpFsrImage output_image;
        vk::UniqueImageView output_image_view;
#else
        VideoCore::UniqueImage intermediary_image;
        vk::UniqueImageView intermediary_image_view;

        VideoCore::UniqueImage output_image;
        vk::UniqueImageView output_image_view;
#endif
    };

    void ResizeAndInvalidate(u32 width, u32 height);
    void CreateImages(Img& img) const;

    vk::Device device{};
    u32 num_images{};
#ifdef UNCHARTED_UWP
    VmaAllocator allocator{};
#endif

    vk::UniqueDescriptorSetLayout descriptor_set_layout{};
    vk::UniqueDescriptorSet easu_descriptor_set{};
    vk::UniqueDescriptorSet rcas_descriptor_set{};
    // UWP (no VK_KHR_push_descriptor): one EASU+RCAS pair per frame slot.
    vk::UniqueDescriptorPool uwp_descriptor_pool{};
    std::vector<vk::DescriptorSet> uwp_descriptor_sets{};
    vk::UniqueSampler sampler{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniquePipeline easu_pipeline{};
    vk::UniquePipeline rcas_pipeline{};

    vk::Extent2D cur_size{};
    u32 cur_image{};
    std::vector<Img> available_imgs;
};

} // namespace Vulkan::HostPasses
