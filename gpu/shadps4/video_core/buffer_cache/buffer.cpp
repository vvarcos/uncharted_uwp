// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <numeric>

#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
extern "C" void* bb_vma_allocator();
extern "C" void bb_heap_check(const char*);

namespace {
// Allocations the allocator refused and the non-fatal path skipped (BB_OOM_NONFATAL). The OOM
// injection test reads it: a log line alone does not prove the policy ran.
std::atomic<u32> g_oom_skips{0};
} // namespace

extern "C" unsigned bb_oom_skip_count() {
    return g_oom_skips.load(std::memory_order_relaxed);
}
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_completion.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_vram.h"

#include <vk_mem_alloc.h>


#ifdef UNCHARTED_UWP
// Staging pool (vk_instance.cpp): one VkDeviceMemory for the non-cached host-visible buffers.
extern "C" VmaPool BbStagingPoolHandle();
extern "C" void BbNoteStagingPoolExhausted();
#endif

namespace VideoCore {

std::string_view BufferTypeName(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
        return "HostUncached";
    case MemoryType::HostCached:
        return "HostCached";
    case MemoryType::Stream:
        return "Stream";
    case MemoryType::DeviceLocal:
        return "DeviceLocal";
    case MemoryType::Sparse:
        return "Sparse";
    default:
        return "Invalid";
    }
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePreferredVmaFlags(MemoryType type) {
    return type != MemoryType::DeviceLocal ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                           : VkMemoryPropertyFlagBits{};
}

[[nodiscard]] VmaAllocationCreateFlags MemoryUsageVmaFlags(MemoryType type) {
    switch (type) {
    case MemoryType::HostUncached:
    case MemoryType::Stream:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT |
               VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    case MemoryType::HostCached:
        return VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    case MemoryType::DeviceLocal:
    default:
        return {};
    }
}

[[nodiscard]] VmaMemoryUsage MemoryUsageVma(MemoryType type) {
    switch (type) {
    case MemoryType::DeviceLocal:
    case MemoryType::Stream:
        return VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    case MemoryType::HostUncached:
    case MemoryType::HostCached:
        return VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    default:
        return VMA_MEMORY_USAGE_UNKNOWN;
    }
}

namespace {
// bbport: every buffer with a device address, alive or destroyed (the last 8192), so a GPU
// crash can name what lived at the faulting address.
struct BdaRecord {
    u64 address, size;
    bool alive;
    char name[40];
};
std::mutex bda_mutex;
std::vector<BdaRecord> bda_records;

void RecordBda(u64 address, u64 size) {
    std::scoped_lock lk{bda_mutex};
    if (bda_records.size() >= 8192) {
        bda_records.erase(bda_records.begin(), bda_records.begin() + 1024);
    }
    bda_records.push_back({address, size, true, {}});
}

void ForgetBda(u64 address) {
    std::scoped_lock lk{bda_mutex};
    for (auto it = bda_records.rbegin(); it != bda_records.rend(); ++it) {
        if (it->address == address && it->alive) {
            it->alive = false;
            return;
        }
    }
}
} // namespace

void NameBda(u64 address, std::string_view name) {
    std::scoped_lock lk{bda_mutex};
    for (auto it = bda_records.rbegin(); it != bda_records.rend(); ++it) {
        if (it->address == address) {
            const size_t n = std::min(name.size(), sizeof(it->name) - 1);
            std::memcpy(it->name, name.data(), n);
            it->name[n] = 0;
            return;
        }
    }
}

void DumpBuffersNear(u64 address) {
    std::scoped_lock lk{bda_mutex};
    std::printf("GPU crash: buffers near 0x%llx (of %zu recorded):\n",
                static_cast<unsigned long long>(address), bda_records.size());
    for (const auto& r : bda_records) {
        const bool inside = address >= r.address && address < r.address + r.size;
        const u64 distance = address >= r.address + r.size ? address - (r.address + r.size)
                             : address < r.address     ? r.address - address
                                                       : 0;
        if (inside || distance < (64ull << 20)) {
            std::printf("GPU crash:   %s 0x%llx-0x%llx %s%s\n", inside ? "HIT " : "near",
                        static_cast<unsigned long long>(r.address),
                        static_cast<unsigned long long>(r.address + r.size),
                        r.alive ? "alive" : "DESTROYED", r.name[0] ? r.name : "");
        }
    }
}

UniqueBuffer::UniqueBuffer(vk::Device device_, VmaAllocator allocator_)
    : device{device_}, allocator{allocator_} {}

UniqueBuffer::~UniqueBuffer() {
    Destroy();
}

void UniqueBuffer::Destroy() {
    if (bda_addr) {
        ForgetBda(bda_addr);
    }
    if (vram_bytes) {
        Vulkan::Vram::buffer_bytes.fetch_sub(vram_bytes, std::memory_order_relaxed);
        vram_bytes = 0;
    }
    if (allocation) {
        vmaDestroyBuffer(allocator, buffer, allocation);
    } else if (buffer) {
        device.destroyBuffer(buffer);
    }
    buffer = VK_NULL_HANDLE;
    allocation = VK_NULL_HANDLE;
    bda_addr = 0;
}

void UniqueBuffer::Create(vk::BufferCreateInfo& buffer_ci, MemoryType mem_type,
                          VmaAllocationInfo* out_alloc_info) {
    if (void* local_allocator = bb_vma_allocator()) {
        allocator = static_cast<VmaAllocator>(local_allocator);
    }
    std::printf("M5 buffer: create allocator=%p size=%llu usage=0x%x\n", static_cast<void*>(allocator), static_cast<unsigned long long>(buffer_ci.size), static_cast<unsigned>(buffer_ci.usage));
    std::fflush(stdout);
    bb_heap_check("before vmaCreateBuffer");
    const bool with_bda = bool(buffer_ci.usage & vk::BufferUsageFlagBits::eShaderDeviceAddress);
    if (mem_type != MemoryType::Sparse) {
        const VmaAllocationCreateFlags bda_flag =
            with_bda ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;
#ifdef UNCHARTED_UWP
        /* bbport: HostUncached and Stream both resolve to the single DEVICE_LOCAL|HOST_VISIBLE|
         * HOST_COHERENT type, so they share one pool and therefore one VkDeviceMemory. HostCached needs
         * HOST_CACHED and stays on the normal allocator. */
        const bool pooled =            mem_type == MemoryType::HostUncached || mem_type == MemoryType::Stream;
        const VmaPool staging_pool = pooled ? BbStagingPoolHandle() : VK_NULL_HANDLE;
#else
        const VmaPool staging_pool = VK_NULL_HANDLE;
#endif
        const VmaAllocationCreateInfo alloc_ci = {
            .flags =
                VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | bda_flag | MemoryUsageVmaFlags(mem_type),
            .usage = MemoryUsageVma(mem_type),
            .requiredFlags = 0,
            .preferredFlags = MemoryUsagePreferredVmaFlags(mem_type),
            .pool = staging_pool,
            .pUserData = nullptr,
        };

        const VkBufferCreateInfo buffer_ci_unsafe = static_cast<VkBufferCreateInfo>(buffer_ci);
        VkBuffer unsafe_buffer{};
        const bool wants_device = alloc_ci.usage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (!wants_device || Vulkan::Vram::Allows(buffer_ci.size)) {
            result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &alloc_ci, &unsafe_buffer,
                                     &allocation, out_alloc_info);
        }
        if (result != VK_SUCCESS && staging_pool != VK_NULL_HANDLE) {
            /* Pool exhausted: fall back to the normal allocator and count it, so blockSize can be sized
             * from data instead of being silently too small. */
#ifdef UNCHARTED_UWP
            BbNoteStagingPoolExhausted();
#endif
            VmaAllocationCreateInfo retry_ci = alloc_ci;
            retry_ci.pool = VK_NULL_HANDLE;
            allocation = nullptr;
            result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &retry_ci, &unsafe_buffer,
                                     &allocation, out_alloc_info);
        }
        if (result != VK_SUCCESS && !Vulkan::Vram::Legacy()) {
            // bbport: the card is full (driver budget): the buffer goes to system memory.
            Vulkan::Vram::failed_allocs.fetch_add(1, std::memory_order_relaxed);
            VmaAllocationCreateInfo host_ci = alloc_ci;
            host_ci.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
            host_ci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            host_ci.pool = VK_NULL_HANDLE; // the host fallback must not use the staging pool
            allocation = nullptr;
            result = vmaCreateBuffer(allocator, &buffer_ci_unsafe, &host_ci, &unsafe_buffer,
                                     &allocation, out_alloc_info);
            if (result == VK_SUCCESS) {
                Vulkan::Vram::host_fallbacks.fetch_add(1, std::memory_order_relaxed);
            }
        }
#ifdef UNCHARTED_UWP
        if (result != VK_SUCCESS) {
            // One structured line with everything needed to tell "budget spent" from "fragmented
            // heap" after the fact, without a debug layer: the request, the VMA heap statistics and
            // the largest free block. vmaCalculateStatistics only walks VMA's own blocks.
            VmaTotalStatistics stats{};
            vmaCalculateStatistics(allocator, &stats);
            std::printf("M5 buffer alloc FAILED: result=%d size=%llu mem_type=%d usage=0x%x "
                        "flags=0x%x req=0x%x pref=0x%x vmaUsage=%d\n",
                        int(result), static_cast<unsigned long long>(buffer_ci.size), int(mem_type),
                        static_cast<unsigned>(buffer_ci.usage), static_cast<unsigned>(alloc_ci.flags),
                        static_cast<unsigned>(alloc_ci.requiredFlags),
                        static_cast<unsigned>(alloc_ci.preferredFlags), int(alloc_ci.usage));
            std::printf("M5 buffer oom: blocks=%llu allocations=%llu used=%llu MiB unused=%llu MiB\n",
                        static_cast<unsigned long long>(stats.total.statistics.blockCount),
                        static_cast<unsigned long long>(stats.total.statistics.allocationCount),
                        static_cast<unsigned long long>(stats.total.statistics.blockBytes >> 20),
                        static_cast<unsigned long long>(stats.total.statistics.blockBytes -
                                                        stats.total.statistics.allocationBytes) >>
                            20);
            std::fflush(stdout);
            // Split the request: a successful vkCreateBuffer with a failing vkAllocateMemory means
            // the resource itself is legal and only the heap is short; the budgets name which heap.
            {
                VkBuffer probe_buffer = VK_NULL_HANDLE;
                const VkResult create_result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateBuffer(
                    static_cast<VkDevice>(device), &buffer_ci_unsafe, nullptr,
                    &probe_buffer);
                if (create_result == VK_SUCCESS) {
                    VkMemoryRequirements req{};
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetBufferMemoryRequirements(
                        static_cast<VkDevice>(device), probe_buffer, &req);
                    uint32_t type_index = UINT32_MAX;
                    const VkResult type_result = vmaFindMemoryTypeIndexForBufferInfo(
                        allocator, &buffer_ci_unsafe, &alloc_ci, &type_index);
                    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
                    vmaGetMemoryProperties(allocator, &properties);
                    VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
                    vmaGetHeapBudgets(allocator, budgets);
                    const uint32_t heap =
                        type_result == VK_SUCCESS && type_index < properties->memoryTypeCount
                            ? properties->memoryTypes[type_index].heapIndex
                            : UINT32_MAX;
                    const VmaBudget* budget = heap < properties->memoryHeapCount ? &budgets[heap] : nullptr;
                    std::printf("M5 buffer split: create=Success req=%llu align=%llu types=0x%08x "
                                "type=%u heap=%u heap_size=%llu budget=%llu usage=%llu\n",
                                static_cast<unsigned long long>(req.size),
                                static_cast<unsigned long long>(req.alignment), req.memoryTypeBits,
                                type_index, heap,
                                budget ? static_cast<unsigned long long>(
                                             properties->memoryHeaps[heap].size) : 0ull,
                                budget ? static_cast<unsigned long long>(budget->budget) : 0ull,
                                budget ? static_cast<unsigned long long>(budget->usage) : 0ull);
                    std::fflush(stdout);
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyBuffer(
                        static_cast<VkDevice>(device), probe_buffer, nullptr);
                } else {
                    std::printf("M5 buffer split: create=%s (allocation not attempted)\n",
                                vk::to_string(vk::Result{create_result}).c_str());
                    std::fflush(stdout);
                }
            }
            // buffer stays null and mapped_data empty). Off by default: production keeps the
            // assert until the fallback ladder exists, so a real OOM is never silently ignored.
            const char* nonfatal = std::getenv("BB_OOM_NONFATAL");
            if (nonfatal != nullptr && nonfatal[0] == '1') {
                g_oom_skips.fetch_add(1, std::memory_order_relaxed);
                std::printf("M5 buffer oom: non-fatal (BB_OOM_NONFATAL), resource skipped; "
                            "skips=%u\n",
                            g_oom_skips.load(std::memory_order_relaxed));
                std::fflush(stdout);
                return;
            }
        }
#endif
        ASSERT_MSG(result == VK_SUCCESS, "Failed allocating buffer with error {}",
                   vk::to_string(vk::Result{result}));
        bb_heap_check("after vmaCreateBuffer");
        buffer = vk::Buffer{unsafe_buffer};
        VkMemoryPropertyFlags placed{};
        vmaGetAllocationMemoryProperties(allocator, allocation, &placed);
        if (placed & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            vram_bytes = buffer_ci.size;
            Vulkan::Vram::buffer_bytes.fetch_add(vram_bytes, std::memory_order_relaxed);
        }
    } else {
        buffer_ci.flags |=
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency;
        buffer = Vulkan::Check(device.createBuffer(buffer_ci));
    }

    if (with_bda) {
        const vk::BufferDeviceAddressInfo bda_info = {
            .buffer = buffer,
        };
        auto bda_result = device.getBufferAddress(bda_info);
        ASSERT_MSG(bda_result != 0, "Failed to get buffer device address");
        bda_addr = bda_result;
        RecordBda(bda_addr, buffer_ci.size);
    }
}

Buffer::Buffer(const Vulkan::Instance& instance, VAddr cpu_addr_, u64 size_bytes_,
               MemoryType mem_type_, std::string_view debug_name)
    : cpu_addr{cpu_addr_}, size_bytes{size_bytes_}, mem_type{mem_type_},
      buffer{instance.GetDevice(), instance.GetAllocator()} {

    vk::BufferCreateInfo buffer_ci = {
        .size = size_bytes,
        .usage = AllFlags,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    // bbport: the arenas are read by the side queue's family too (BufferCache read-backs).
    const std::array families = {instance.GetGraphicsQueueFamilyIndex(),
                                 instance.GetSideQueueFamilyIndex()};
    if (mem_type == MemoryType::Sparse && families[0] != families[1]) {
        buffer_ci.sharingMode = vk::SharingMode::eConcurrent;
        buffer_ci.queueFamilyIndexCount = static_cast<u32>(families.size());
        buffer_ci.pQueueFamilyIndices = families.data();
    }
    VmaAllocationInfo alloc_info{};
#ifdef UNCHARTED_UWP
    if (size_bytes >= 256_MB) {
        std::printf("M5 buffer large: name='%s' type=%d size=%llu MiB cpu=0x%llx\n",
                    debug_name.empty() ? "?" : std::string(debug_name).c_str(), int(mem_type),
                    static_cast<unsigned long long>(size_bytes >> 20),
                    static_cast<unsigned long long>(cpu_addr));
        std::fflush(stdout);
    }
#endif
    // A zero-byte buffer is not a legal VkBufferCreateInfo: the driver answers
    // VK_ERROR_INITIALIZATION_FAILED (-3) and once crashed reading -1, which took the whole boot
    // down with no exception record. Nothing reads a zero-byte staging buffer, so keep it legal.
    if (buffer_ci.size == 0) {
        std::printf("M5 buffer: zero-byte request ('%s' type %d) raised to 64 KiB\n",
                    debug_name.empty() ? "?" : std::string(debug_name).c_str(), int(mem_type));
        std::fflush(stdout);
        buffer_ci.size = 64_KB;
    }
    buffer.Create(buffer_ci, mem_type, &alloc_info);
    bb_heap_check("Buffer ctor after Create");
    // The non-fatal OOM path leaves the buffer null: the remaining steps (debug name, memory
    // properties, mapped span) all assume a live handle, so the resource is skipped here.
    if (!buffer.buffer) {
        std::printf("M5 buffer: skipped '%s' size=%llu type=%d (no allocation)\n",
                    debug_name.empty() ? "?" : std::string(debug_name).c_str(),
                    static_cast<unsigned long long>(size_bytes), int(mem_type));
        std::fflush(stdout);
        return;
    }

    const auto device = instance.GetDevice();
    if (buffer.bda_addr) {
        NameBda(buffer.bda_addr, !debug_name.empty() ? debug_name : BufferTypeName(mem_type));
    }
    if (!debug_name.empty()) {
        Vulkan::SetObjectName(device, Handle(), debug_name);
    } else {
        Vulkan::SetObjectName(device, Handle(), "Buffer {:#x}:{:#x}", cpu_addr, size_bytes);
    }

    if (Vulkan::Vram::Log() && size_bytes >= (8u << 20)) {
        std::printf("VRAM: buffer '%s' %s %llu MiB (vram counted %llu MiB)\n",
                    debug_name.empty() ? "?" : std::string(debug_name).c_str(),
                    std::string(BufferTypeName(mem_type)).c_str(), (unsigned long long)(size_bytes >> 20),
                    (unsigned long long)(buffer.vram_bytes >> 20));
    }
    if (mem_type != MemoryType::Sparse) {
        VkMemoryPropertyFlags property_flags{};
        vmaGetAllocationMemoryProperties(instance.GetAllocator(), buffer.allocation,
                                         &property_flags);
        if (alloc_info.pMappedData) {
            mapped_data = std::span<u8>{std::bit_cast<u8*>(alloc_info.pMappedData), size_bytes};
        }
        is_coherent = property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
}

void Buffer::Flush(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaFlushAllocation(buffer.allocator, buffer.allocation, offset, size);
}

void Buffer::Invalidate(u64 offset, u64 size) {
    if (mapped_data.empty() || is_coherent) {
        return;
    }
    vmaInvalidateAllocation(buffer.allocator, buffer.allocation, offset, size);
}

constexpr u64 WATCHES_INITIAL_RESERVE = 0x100;
constexpr u64 WATCHES_RESERVE_CHUNK = 0x100;

StreamBuffer::StreamBuffer(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler_,
                           MemoryType mem_type, u64 size_bytes)
    : Buffer{instance, 0, size_bytes, mem_type}, scheduler{scheduler_},
      non_coherent_atom_size{instance.NonCoherentAtomSize()} {
    ReserveWatches(current_watches, WATCHES_INITIAL_RESERVE);
    ReserveWatches(previous_watches, WATCHES_INITIAL_RESERVE);
    Vulkan::SetObjectName(instance.GetDevice(), Handle(), "StreamBuffer({}):{:#x}",
                          BufferTypeName(mem_type), size_bytes);
    bb_heap_check("StreamBuffer ctor end");
}

bool StreamBuffer::PrepareMap(u64 size, u64 alignment, bool allow_wait) {
    if (!mapped_data.empty() && !is_coherent) {
        size = Common::AlignUp(size, non_coherent_atom_size);
        alignment =
            alignment > 0 ? std::lcm(alignment, non_coherent_atom_size) : non_coherent_atom_size;
    }

    if (size > this->size_bytes) {
        return false;
    }

    if (alignment > 0) {
        offset = Common::AlignUp(offset, alignment);
    }

    if (offset + size > this->size_bytes) {
        // The buffer would overflow, save the amount of used watches and reset the state.
        invalidation_mark = current_watch_cursor;
        current_watch_cursor = 0;
        offset = 0;

        // Swap watches and reset waiting cursors.
        std::swap(previous_watches, current_watches);
        wait_cursor = 0;
        wait_bound = 0;
    }

    if (!WaitPendingOperations(offset + size, allow_wait)) {
        return false;
    }

    mapped_size = size;
    return true;
}

std::pair<u8*, u64> StreamBuffer::Map(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return {nullptr, 0};
    }
    u8* const data = mapped_data.empty() ? nullptr : mapped_data.data() + offset;
    return {data, offset};
}

void StreamBuffer::Commit() {
    if (mem_type == MemoryType::HostCached) {
        Invalidate(offset, mapped_size);
    } else {
        Flush(offset, mapped_size);
    }
    AdvanceAndWatch();
}

std::optional<u64> StreamBuffer::Reserve(u64 size, u64 alignment, bool allow_wait) {
    if (!PrepareMap(size, alignment, allow_wait)) {
        return std::nullopt;
    }
    const u64 reserved_offset = offset;
    AdvanceAndWatch();
    return reserved_offset;
}

void StreamBuffer::AdvanceAndWatch() {
    offset += mapped_size;
    const u64 tick = scheduler.CurrentTick();
    last_tick = tick;

    // Extend the last watch if it belongs to the same tick.
    if (current_watch_cursor != 0 && current_watches[current_watch_cursor - 1].tick == tick) {
        current_watches[current_watch_cursor - 1].upper_bound = offset;
        return;
    }

    if (current_watch_cursor + 1 >= current_watches.size()) {
        // Ensure that there are enough watches.
        ReserveWatches(current_watches, WATCHES_RESERVE_CHUNK);
    }

    auto& watch = current_watches[current_watch_cursor++];
    watch.upper_bound = offset;
    watch.tick = tick;
}

void StreamBuffer::ReserveWatches(std::vector<Watch>& watches, std::size_t grow_size) {
    watches.resize(watches.size() + grow_size);
}

bool StreamBuffer::WaitPendingOperations(u64 requested_upper_bound, bool allow_wait) {
    // bbport: the first rule is the only one that protects a recorded-but-not-submitted command
    // buffer: a buffer referenced while recording may already be read by that command buffer, and
    // reusing it now would let the GPU see overwritten bytes. The port measured a ~3.7 ms GPU
    // window on this device, so this is not a theoretical hazard.
    if (references.load(std::memory_order_acquire) != 0) {
        return false;
    }
    // A buffer stamped by a submit can only be reused once that value completed on its own domain.
    if (submitted_value != 0) {
        auto& registry = Vulkan::CompletionRegistry::Instance();
        const Vulkan::CompletionToken token{.domain = Vulkan::CompletionDomainId::Main,
                                            .value = submitted_value};
        if (!registry.IsComplete(token)) {
            if (!allow_wait) {
                return false;
            }
            scheduler.Wait(submitted_value);
        }
        // Invariant, always compiled: a block is never released before the submit that references
        // it completed on its domain. Logged and counted rather than asserted, so a violation is
        // evidence instead of a process death in the middle of a 600 s run.
        if (!registry.IsComplete(token)) {
            static std::atomic<u32> violations{0};
            const u32 seen = violations.fetch_add(1, std::memory_order_relaxed) + 1;
            if (seen <= 8 || (seen % 100) == 0) {
                std::printf("M5 ring INVARIANT: reuse with completed=%llu < value=%llu (violations=%u)\n",
                            static_cast<unsigned long long>(registry.LastCompleted(token.domain)),
                            static_cast<unsigned long long>(token.value), seen);
                std::fflush(stdout);
            }
        }
        submitted_value = 0;
        return true;
    }
    if (!invalidation_mark) {
        return true;
    }
    while (requested_upper_bound > wait_bound && wait_cursor < *invalidation_mark) {
        auto& watch = previous_watches[wait_cursor];
        if (!scheduler.IsFree(watch.tick)) {
            if (!allow_wait) {
                return false;
            }
            scheduler.Wait(watch.tick);
        }
        wait_bound = watch.upper_bound;
        ++wait_cursor;
    }
    return true;
}

void StreamBuffer::OnSubmitted(u64 value) noexcept {
    references.store(0, std::memory_order_release);
    submitted_value = value;
    Vulkan::CompletionRegistry::Instance().Submitted(Vulkan::CompletionDomainId::Main, value);
}


} // namespace VideoCore

namespace Vulkan {

void SubmitScope::Commit(u64 signal_value) {
    if (committed) {
        return;
    }
    committed = true;
    for (auto* buffer : buffers) {
        if (buffer != nullptr) {
            buffer->OnSubmitted(signal_value);
        }
    }
    CompletionRegistry::Instance().Submitted(domain, signal_value);
    buffers.clear();
}

void SubmitScope::Abort() {
    if (committed) {
        return;
    }
    committed = true;
    for (auto* buffer : buffers) {
        if (buffer != nullptr) {
            buffer->OnScopeAborted();
        }
    }
    buffers.clear();
}

} // namespace Vulkan

