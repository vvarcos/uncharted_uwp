// SPDX-License-Identifier: GPL-2.0-or-later
//
// bbport: which caller is creating this device memory. VMA's pfnAllocate receives the pUserData of the
// CALLBACK (global), not the pUserData of the allocation, so a per-allocation tag cannot come from
// there. The allocation happens synchronously inside vmaCreate*, on the calling thread, so a
// thread-local set by a guard at the call site names the owner reliably.
#pragma once

#ifdef UNCHARTED_UWP
namespace Vulkan {
inline thread_local const char* g_memory_tag = "?";
struct MemoryTag {
    explicit MemoryTag(const char* tag) { g_memory_tag = tag; }
    ~MemoryTag() { g_memory_tag = "?"; }
    MemoryTag(const MemoryTag&) = delete;
    MemoryTag& operator=(const MemoryTag&) = delete;
};
} // namespace Vulkan
#define BB_MEMORY_TAG(name) ::Vulkan::MemoryTag bb_memory_tag_guard_{name}
#else
#define BB_MEMORY_TAG(name)
#endif
