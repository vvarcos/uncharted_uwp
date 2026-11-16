// M5 isolated descriptor-set lifetime diagnostic on the real headless renderer device.
// No guest, Liverpool, push descriptors, presentation, or GPU IRQs.
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include "video_core/renderer_vulkan/vk_instance.h"
#include <vk_mem_alloc.h>
#include "video_core/host_shaders/m5_descriptor_comp.h"

namespace {
void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void Stage(const char* stage) {
    std::printf("M5 descriptor stage: %s\n", stage);
    std::fflush(stdout);
}
struct StorageBuffer {
    VmaAllocator allocator{};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    ~StorageBuffer() { if (buffer) vmaDestroyBuffer(allocator, buffer, allocation); }
};

int TestDescriptorLifetime() {
    Vulkan::Instance renderer{-1, false};
    const auto device = renderer.GetDevice();
    const VmaAllocator allocator = renderer.GetAllocator();
    Require(device && allocator, "renderer logical device/VMA unavailable");
    const auto limits = renderer.GetPhysicalDevice().getProperties().limits;
    const auto storage_alignment = std::max<uint32_t>(
        16, static_cast<uint32_t>(limits.minStorageBufferOffsetAlignment));
    const auto uniform_alignment = std::max<uint32_t>(
        16, static_cast<uint32_t>(limits.minUniformBufferOffsetAlignment));
    std::printf("M5 descriptor alignments: storage=%u uniform=%u\n",
                storage_alignment, uniform_alignment);
    StorageBuffer storage{.allocator = allocator};
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4ull * storage_alignment,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo alloc_info{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    Require(vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &storage.buffer,
                            &storage.allocation, nullptr) == VK_SUCCESS, "storage buffer allocation failed");
    void* mapped = nullptr;
    Require(vmaMapMemory(allocator, storage.allocation, &mapped) == VK_SUCCESS && mapped,
            "storage buffer map failed");
    std::memset(mapped, 0, static_cast<size_t>(buffer_info.size));
    Require(vmaFlushAllocation(allocator, storage.allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
            "storage buffer flush failed");
    vmaUnmapMemory(allocator, storage.allocation);
    StorageBuffer parameters{.allocator = allocator};
    const VkBufferCreateInfo parameter_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 3ull * uniform_alignment,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    Require(vmaCreateBuffer(allocator, &parameter_info, &alloc_info, &parameters.buffer,
                            &parameters.allocation, nullptr) == VK_SUCCESS,
            "descriptor uniform buffer allocation failed");
    mapped = nullptr;
    Require(vmaMapMemory(allocator, parameters.allocation, &mapped) == VK_SUCCESS && mapped,
            "descriptor uniform buffer map failed");
    std::memset(mapped, 0, static_cast<size_t>(parameter_info.size));
    for (uint32_t i = 0; i < 3; ++i) {
        const std::array<uint32_t, 3> values{0x10203040u, 0x50607080u, 0x90A0B0C0u};
        std::memcpy(static_cast<uint8_t*>(mapped) + i * uniform_alignment,
                    &values[i], sizeof(uint32_t));
    }
    Require(vmaFlushAllocation(allocator, parameters.allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
            "descriptor uniform buffer flush failed");
    vmaUnmapMemory(allocator, parameters.allocation);
    Stage("buffers ready");

    const auto [shader_result, shader] = device.createShaderModuleUnique(
        {.codeSize = sizeof(M5_DESCRIPTOR_COMP), .pCode = M5_DESCRIPTOR_COMP});
    Require(shader_result == vk::Result::eSuccess && bool(shader), "descriptor shader failed");
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{.binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
        vk::DescriptorSetLayoutBinding{.binding = 1,
            .descriptorType = vk::DescriptorType::eUniformBuffer,
            .descriptorCount = 1, .stageFlags = vk::ShaderStageFlagBits::eCompute},
    };
    const vk::DescriptorSetLayoutCreateInfo set_layout_info{
        .bindingCount = static_cast<uint32_t>(bindings.size()), .pBindings = bindings.data(),
    };
    const auto [set_layout_result, set_layout] = device.createDescriptorSetLayoutUnique(set_layout_info);
    Require(set_layout_result == vk::Result::eSuccess && bool(set_layout),
            "standard descriptor set layout failed");
    Stage("set layout ready");
    const vk::DescriptorSetLayout raw_layout = *set_layout;
    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 1, .pSetLayouts = &raw_layout,
    };
    const auto [layout_result, pipeline_layout] = device.createPipelineLayoutUnique(layout_info);
    if (layout_result != vk::Result::eSuccess || !pipeline_layout) {
        const auto limits = renderer.GetPhysicalDevice().getProperties().limits;
        std::printf("M5 descriptor layout result=%d maxPush=%u maxBoundSets=%u\n",
                    static_cast<int>(layout_result), limits.maxPushConstantsSize,
                    limits.maxBoundDescriptorSets);
    }
    Require(layout_result == vk::Result::eSuccess && bool(pipeline_layout),
            "descriptor pipeline layout failed");
    const vk::ComputePipelineCreateInfo pipeline_info{
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = *shader,
                  .pName = "main"},
        .layout = *pipeline_layout,
    };
    const auto [pipeline_result, pipeline] = device.createComputePipelineUnique({}, pipeline_info);
    if (pipeline_result != vk::Result::eSuccess || !pipeline) {
        std::printf("M5 descriptor compute pipeline result=%d\n", static_cast<int>(pipeline_result));
    }
    Require(pipeline_result == vk::Result::eSuccess && bool(pipeline),
            "descriptor compute pipeline failed");
    Stage("compute pipeline ready");
    const auto [cmd_pool_result, command_pool] = device.createCommandPoolUnique(
        {.queueFamilyIndex = renderer.GetGraphicsQueueFamilyIndex()});
    Require(cmd_pool_result == vk::Result::eSuccess && bool(command_pool),
            "descriptor command pool failed");
    const auto [cmd_result, commands] = device.allocateCommandBuffersUnique(
        {.commandPool = *command_pool, .level = vk::CommandBufferLevel::ePrimary,
         .commandBufferCount = 3});
    if (cmd_result != vk::Result::eSuccess || commands.size() != 3) {
        std::printf("M5 descriptor command allocation result=%d count=%zu\n",
                    static_cast<int>(cmd_result), commands.size());
    }
    Require(cmd_result == vk::Result::eSuccess && commands.size() == 3,
            "descriptor command buffers failed");
    Stage("three command buffers ready");
    const std::array pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 2},
        vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 2},
    };
    const vk::DescriptorPoolCreateInfo pool_info{
        .maxSets = 2, .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };
    const auto [pool_result, descriptor_pool] = device.createDescriptorPoolUnique(pool_info);
    Require(pool_result == vk::Result::eSuccess && bool(descriptor_pool),
            "standard descriptor pool failed");
    const std::array layouts{*set_layout, *set_layout};
    const vk::DescriptorSetAllocateInfo set_info{
        .descriptorPool = *descriptor_pool,
        .descriptorSetCount = 2,
        .pSetLayouts = layouts.data(),
    };
    const auto [sets_result, sets] = device.allocateDescriptorSets(set_info);
    Require(sets_result == vk::Result::eSuccess && sets.size() == 2,
            "standard descriptor sets allocation failed");
    Stage("two descriptor sets allocated");
    const auto update_set = [&](uint32_t set_index, uint32_t word_index) {
        const vk::DescriptorBufferInfo info{
            .buffer = vk::Buffer{storage.buffer},
            .offset = word_index * storage_alignment,
            .range = storage_alignment,
        };
        const vk::DescriptorBufferInfo uniform_info{
            .buffer = vk::Buffer{parameters.buffer},
            .offset = word_index * uniform_alignment,
            .range = uniform_alignment,
        };
        const std::array writes{
            vk::WriteDescriptorSet{.dstSet = sets[set_index], .dstBinding = 0,
                .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &info},
            vk::WriteDescriptorSet{.dstSet = sets[set_index], .dstBinding = 1,
                .descriptorCount = 1, .descriptorType = vk::DescriptorType::eUniformBuffer,
                .pBufferInfo = &uniform_info},
        };
        device.updateDescriptorSets(writes, {});
    };
    update_set(0, 0);
    update_set(1, 1);
    Stage("initial descriptor writes complete");

    const auto record = [&](vk::CommandBuffer command, vk::DescriptorSet set,
                            bool host_barrier) {
        Stage("record begin requested");
        Require(command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) ==
                    vk::Result::eSuccess, "descriptor command begin failed");
        Stage("command begun; binding pipeline");
        command.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        Stage("pipeline bound; binding descriptor set");
        command.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeline_layout, 0,
                                   std::array{set}, {});
        Stage("descriptor set bound; dispatching");
        const auto family = renderer.GetGraphicsQueueFamilyIndex();
        const auto flags = renderer.GetPhysicalDevice().getQueueFamilyProperties().at(family).queueFlags;
        const bool has_dispatch = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdDispatch != nullptr;
        const bool compute_queue = bool(flags & vk::QueueFlagBits::eCompute);
        std::printf("M5 descriptor dispatch preflight: entry=%u compute_queue=%u family=%u\n",
                    has_dispatch, compute_queue, family);
        std::fflush(stdout);
        Require(has_dispatch && compute_queue, "vkCmdDispatch unavailable on graphics queue");
        command.dispatch(1, 1, 1);
        Stage("dispatch recorded");
        if (host_barrier) {
            const vk::BufferMemoryBarrier barrier{
                .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                .dstAccessMask = vk::AccessFlagBits::eHostRead,
                .buffer = vk::Buffer{storage.buffer}, .size = VK_WHOLE_SIZE,
            };
            command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                    vk::PipelineStageFlagBits::eHost, {}, {},
                                    std::array{barrier}, {});
        }
        Stage("command end requested");
        Require(command.end() == vk::Result::eSuccess, "descriptor command end failed");
        Stage("command ended");
    };
    record(*commands[0], sets[0], false);
    record(*commands[1], sets[1], true);
    Stage("two command buffers recorded");
    const auto [fence_result, fence] = device.createFenceUnique({});
    Require(fence_result == vk::Result::eSuccess && bool(fence), "descriptor fence failed");
    const std::array first_commands{*commands[0], *commands[1]};
    const vk::SubmitInfo first_submit{
        .commandBufferCount = 2, .pCommandBuffers = first_commands.data(),
    };
    Require(renderer.GetGraphicsQueue().submit(std::array{first_submit}, *fence) ==
                vk::Result::eSuccess, "two descriptor command buffers submit failed");
    Require(device.waitForFences(std::array{*fence}, VK_TRUE, 5000000000ull) ==
                vk::Result::eSuccess, "first descriptor fence timeout/failure");
    Stage("first fence signaled");
    // Only after the fence: reuse the first set with an updated buffer offset.
    update_set(0, 2);
    record(*commands[2], sets[0], true);
    const auto [fence2_result, second_fence] = device.createFenceUnique({});
    Require(fence2_result == vk::Result::eSuccess && bool(second_fence),
            "second descriptor fence failed");
    const vk::CommandBuffer third_command = *commands[2];
    const vk::SubmitInfo second_submit{
        .commandBufferCount = 1, .pCommandBuffers = &third_command,
    };
    Require(renderer.GetGraphicsQueue().submit(std::array{second_submit}, *second_fence) ==
                vk::Result::eSuccess, "reused descriptor submit failed");
    Require(device.waitForFences(std::array{*second_fence}, VK_TRUE, 5000000000ull) ==
                vk::Result::eSuccess, "second descriptor fence timeout/failure");
    Stage("second fence signaled");
    Require(vmaInvalidateAllocation(allocator, storage.allocation, 0, VK_WHOLE_SIZE) == VK_SUCCESS,
            "descriptor storage cache invalidate failed");
    mapped = nullptr;
    Require(vmaMapMemory(allocator, storage.allocation, &mapped) == VK_SUCCESS && mapped,
            "descriptor readback map failed");
    std::array<uint32_t, 4> result{};
    for (uint32_t i = 0; i < result.size(); ++i) {
        std::memcpy(&result[i], static_cast<const uint8_t*>(mapped) + i * storage_alignment,
                    sizeof(uint32_t));
    }
    vmaUnmapMemory(allocator, storage.allocation);
    std::printf("M5 renderer descriptor readback: %08X %08X %08X %08X\n",
                result[0], result[1], result[2], result[3]);
    Require(result == std::array<uint32_t, 4>{0x10203040u, 0x50607080u,
                                               0x90A0B0C0u, 0u},
            "descriptor set isolation/reuse mismatch");
    std::puts("M5 renderer descriptor sets PASS: two command buffers, fence-then-update/reuse; "
              "no push descriptors or guest frame");
    return 0;
}
} // namespace

extern "C" int bbgpu_uwp_renderer_descriptor_smoke(void) {
    try { return TestDescriptorLifetime(); }
    catch (const std::exception& error) {
        std::printf("M5 renderer descriptor sets FAILED: %s\n", error.what());
        return 1;
    }
}
#endif
