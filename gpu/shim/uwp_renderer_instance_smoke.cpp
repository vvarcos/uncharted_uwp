// Explicit M5 experiments: the renderer's Vulkan instance and, separately,
// its logical device/VMA. Neither mode starts the guest or draws a game frame.
#ifdef UNCHARTED_M5_HEADLESS_INSTANCE
#include <cstdio>
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include <vk_mem_alloc.h>

extern "C" int bbgpu_uwp_headless_instance_smoke(void) {
    std::puts("M5 renderer instance smoke: packaged Dozen, headless Vulkan 1.2; guest renderer disabled");
    auto instance=Vulkan::CreateInstance(Frontend::WindowSystemType::Headless,false,false);
    if(!instance) return 1;
    auto devices=instance->enumeratePhysicalDevices();
    if(devices.result!=vk::Result::eSuccess || devices.value.empty()) return 2;
    auto properties=devices.value.front().getProperties();
    std::printf("M5 renderer CreateInstance PASS: %s, device API %u.%u; no device or game frame\n",
                properties.deviceName.data(),VK_VERSION_MAJOR(properties.apiVersion),
                VK_VERSION_MINOR(properties.apiVersion));
    return 0;
}

#ifdef UNCHARTED_M5_HEADLESS_DEVICE
extern "C" int bbgpu_uwp_renderer_class_device_smoke(void) {
    std::puts("M5 renderer class device: constructing Vulkan::Instance headless (no guest)");
    Vulkan::Instance renderer{-1, false};
    if (!renderer.GetDevice() || !renderer.GetAllocator()) return 1;
    std::printf("M5 renderer class device PASS: API %u.%u; no Liverpool, guest frame or gameplay\n",
                VK_VERSION_MAJOR(renderer.ApiVersion()), VK_VERSION_MINOR(renderer.ApiVersion()));
    return 0;
}

// Isolated negotiation using the renderer's actual packaged-Dozen instance factory.
// This is deliberately NOT Vulkan::Instance::CreateDevice, Liverpool or a guest frame.
extern "C" int bbgpu_uwp_headless_device_smoke(void) {
    auto instance=Vulkan::CreateInstance(Frontend::WindowSystemType::Headless,false,false);
    if(!instance) return 1;
    auto devices=instance->enumeratePhysicalDevices();
    if(devices.result!=vk::Result::eSuccess || devices.value.empty()) return 2;
    const vk::PhysicalDevice physical=devices.value.front();
    const auto properties=physical.getProperties();
    if(properties.apiVersion<VK_API_VERSION_1_2) return 3;
    const auto available=physical.enumerateDeviceExtensionProperties();
    if(available.result!=vk::Result::eSuccess) return 4;
    const auto has_extension=[&](const char* name) {
        return std::any_of(available.value.begin(),available.value.end(),[&](const auto& ext) {
            return std::string_view{ext.extensionName.data()}==name;
        });
    };
    constexpr std::array required{VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                                  VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};
    for(const char* name:required) {
        if(!has_extension(name)) {
            std::printf("M5 renderer device preflight: missing %s\n",name);
            return 5;
        }
    }
    const auto feature_chain=physical.getFeatures2<
        vk::PhysicalDeviceFeatures2,vk::PhysicalDeviceVulkan12Features,
        vk::PhysicalDeviceDynamicRenderingFeaturesKHR,
        vk::PhysicalDeviceSynchronization2FeaturesKHR>();
    const auto f12=feature_chain.get<vk::PhysicalDeviceVulkan12Features>();
    const auto dynamic=feature_chain.get<vk::PhysicalDeviceDynamicRenderingFeaturesKHR>();
    const auto sync=feature_chain.get<vk::PhysicalDeviceSynchronization2FeaturesKHR>();
    const bool sparse=feature_chain.get<vk::PhysicalDeviceFeatures2>().features.sparseBinding;
    std::printf("M5 renderer device preflight: API=%u.%u dynamic=%u sync2=%u BDA=%u sparse=%u push=%u robustness2=%u\n",
                VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion),
                dynamic.dynamicRendering,sync.synchronization2,f12.bufferDeviceAddress,sparse,
                has_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME),
                has_extension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME));
    if(!dynamic.dynamicRendering || !sync.synchronization2) return 6;
    const auto queue_families=physical.getQueueFamilyProperties();
    const auto it=std::find_if(queue_families.begin(),queue_families.end(),[](const auto& family) {
        return bool(family.queueFlags & vk::QueueFlagBits::eGraphics);
    });
    if(it==queue_families.end()) return 7;
    const auto family=static_cast<uint32_t>(it-queue_families.begin());
    float priority=1.0f;
    const vk::DeviceQueueCreateInfo queue_info{.queueFamilyIndex=family,.queueCount=1,
                                                .pQueuePriorities=&priority};
    vk::StructureChain create_chain{
        vk::DeviceCreateInfo{.queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info,
                             .enabledExtensionCount=static_cast<uint32_t>(required.size()),
                             .ppEnabledExtensionNames=required.data()},
        vk::PhysicalDeviceFeatures2{},
        vk::PhysicalDeviceVulkan12Features{.timelineSemaphore=f12.timelineSemaphore},
        vk::PhysicalDeviceDynamicRenderingFeaturesKHR{.dynamicRendering=dynamic.dynamicRendering},
        vk::PhysicalDeviceSynchronization2FeaturesKHR{.synchronization2=sync.synchronization2},
    };
    auto [device_result,device]=physical.createDeviceUnique(create_chain.get());
    if(device_result!=vk::Result::eSuccess || !device) {
        std::printf("M5 renderer device creation failed: %d\n",static_cast<int>(device_result));
        return 8;
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(*device);
    const VmaVulkanFunctions functions{
        .vkGetInstanceProcAddr=VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr=VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
    };
    const VmaAllocatorCreateInfo allocator_info{
        .physicalDevice=physical,.device=*device,.pVulkanFunctions=&functions,
        .instance=*instance,.vulkanApiVersion=VK_API_VERSION_1_2,
    };
    VmaAllocator allocator=nullptr;
    const VkResult allocator_result=vmaCreateAllocator(&allocator_info,&allocator);
    if(allocator_result!=VK_SUCCESS) {
        std::printf("M5 renderer VMA creation failed: %d\n",allocator_result);
        return 9;
    }
    VkBuffer buffer=VK_NULL_HANDLE;
    VmaAllocation allocation=nullptr;
    const VkBufferCreateInfo buffer_info{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                         .size=4096,.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    const VmaAllocationCreateInfo allocation_info{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    const VkResult allocation_result=vmaCreateBuffer(allocator,&buffer_info,&allocation_info,
                                                     &buffer,&allocation,nullptr);
    if(allocation_result==VK_SUCCESS) vmaDestroyBuffer(allocator,buffer,allocation);
    vmaDestroyAllocator(allocator);
    if(allocation_result!=VK_SUCCESS) {
        std::printf("M5 renderer VMA buffer creation failed: %d\n",allocation_result);
        return 10;
    }
    std::printf("M5 renderer logical device PASS: Dozen API 1.2, graphics queue %u, VMA buffer 4096 bytes; "
                "no Vulkan::Instance device, guest frame or gameplay\n",family);
    return 0;
}
#endif
#endif
