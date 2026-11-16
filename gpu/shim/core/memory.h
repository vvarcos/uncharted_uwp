// bbport: the MemoryManager surface used by the video core and GnmDriver.
// Guest memory lives at identical host addresses; the C runtime owns the VMAs.
#pragma once
#include "common/types.h"
#include "common/singleton.h"
#include "core/address_space.h"

namespace Vulkan { class Rasterizer; }

extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);

namespace Core {
class MemoryManager {
public:
    void SetRasterizer(Vulkan::Rasterizer* rasterizer_);
    AddressSpace& GetAddressSpace() { return impl; }
    /// bbport: is this guest range addressable by the GPU? The previous version only compared
    /// `addr + size` with the 40-bit limit, which wrapped for a high address and said yes, and it
    /// also said yes for any address without a mapping behind it. The predicate now is: inside the
    /// 40-bit GPU space (overflow-safe), page aligned, and fully covered by a guest-mapped region.
    /// Committed is deliberately NOT required: the port materialises pages on first touch, so
    /// demanding commit here would break the commit-on-touch model that the GPU relies on.
    bool IsValidGpuMapping(VAddr virtual_addr, u64 size) {
        constexpr u64 GpuLimit = 0x100'0000'0000ULL; // the PS4's 40-bit GPU address space
        constexpr u64 PageSize = 4096;
        // Video-core callers use size==0 as a probe for the page containing the address.
        // Preserve that contract instead of rejecting every texture/vertex descriptor probe.
        if (size == 0) {
            size = 1;
        }
        if (virtual_addr >= GpuLimit || size > GpuLimit - virtual_addr) {
            return false;
        }
        const u64 page = virtual_addr & ~(PageSize - 1);
        const u64 span = (virtual_addr - page) + size;
        const u64 rounded = (span + PageSize - 1) & ~(PageSize - 1);
        return runtime_memory_is_mapped(static_cast<uintptr_t>(page), rounded) != 0;
    }
    u64 ClampRangeSize(VAddr virtual_addr, u64 size);
    void CopySparseMemory(VAddr source, u8* dest, u64 size);
    bool TryWriteBacking(void* address, const void* data, u64 size);
    /// CPU wrote guest memory outside page tracking (e.g. decoded video frames).
    void InvalidateMemory(VAddr address, u64 size);
    Vulkan::Rasterizer* GetRasterizer() const { return rasterizer; }

private:
    AddressSpace impl;
    Vulkan::Rasterizer* rasterizer{};
};
// bbport: the memory manager is owned by the executable; a module that hosts renderer code (the
// diagnostic DLL) aliases the host object instead of constructing a second, empty one.
class Memory {
public:
    static MemoryManager* Instance() {
        return host_instance != nullptr ? host_instance : &Local();
    }
    static void SetHost(MemoryManager* instance) {
        host_instance = instance;
    }

private:
    static MemoryManager& Local() {
        static MemoryManager local;
        return local;
    }
    static inline MemoryManager* host_instance = nullptr;
};
} // namespace Core
