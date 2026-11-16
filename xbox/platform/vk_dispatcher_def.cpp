// The executable references Vulkan-Hpp's default dispatcher directly (probe/present bridge).
// bbgpu.dll has its own copy; this definition keeps the executable self-contained without
// pulling the renderer archive back in.
#include "video_core/renderer_vulkan/vk_common.h"

VULKAN_HPP_DEFAULT_DISPATCHER_TYPE vk::detail::defaultDispatchLoaderDynamic{};
