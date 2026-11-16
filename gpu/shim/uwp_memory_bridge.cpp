// Bridge for the diagnostic DLL: expose the host's MemoryManager without dragging the memory
// header (and Boost) into the executable translation units.
#include "core/memory.h"

extern "C" void* uncharted_uwp_memory_manager() {
    return static_cast<void*>(Core::Memory::Instance());
}

// The VMA allocator cannot be shared across modules: it stores device function pointers bound to
// the module that created it. The DLL therefore builds its own allocator from raw handles.
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace {
VmaAllocator g_local_allocator = nullptr;
}

extern "C" void* bb_vma_allocator() {
    return g_local_allocator;
}

extern "C" int bb_vma_set_handles(VkInstance instance, VkPhysicalDevice physical,
                                  VkDevice device, PFN_vkGetInstanceProcAddr gipa,
                                  PFN_vkGetDeviceProcAddr gdpa) {
    if (g_local_allocator != nullptr) {
        return 1;
    }
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = gipa;
    functions.vkGetDeviceProcAddr = gdpa;
    VmaAllocatorCreateInfo info{};
    info.instance = instance;
    info.physicalDevice = physical;
    info.device = device;
    info.pVulkanFunctions = &functions;
    const VkResult result = vmaCreateAllocator(&info, &g_local_allocator);
    return result == VK_SUCCESS ? 0 : static_cast<int>(result);
}
