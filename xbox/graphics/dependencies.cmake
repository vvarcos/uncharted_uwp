# M2 probe needs only the pinned Vulkan headers. Full bbgpu dependencies are introduced when
# UNCHARTED_LINK_RENDERER is enabled after the isolated Dozen/CoreWindow bridge is validated.
if(NOT EXISTS "${UNCHARTED_DEPS_ROOT}/vulkan-headers/include/vulkan/vulkan.h")
  message(FATAL_ERROR "Pinned Vulkan headers missing from ${UNCHARTED_DEPS_ROOT}")
endif()
add_library(VulkanHeaders INTERFACE)
add_library(Vulkan::Headers ALIAS VulkanHeaders)
target_include_directories(VulkanHeaders INTERFACE "${UNCHARTED_DEPS_ROOT}/vulkan-headers/include")
