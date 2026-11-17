// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bbport_toggles.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include <vk_mem_alloc.h>
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_vram.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// bbport: staging memory is populated (allocated and cleared) by the kernel on first CPU access,
// ~2 ms per 16 MiB block. Streaming bursts a few seconds apart re-created the blocks trimmed
// in between and copies into them ran at 0.3 GB/s (stutter). Blocks now live 30 s idle, the
// upload ring keeps BB_STAGING_KEEP_MB (512) and is populated at startup.
constexpr u64 RING_IDLE_FRAMES = 3000;
constexpr u64 LARGE_IDLE_FRAMES = 3000;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
#ifdef UNCHARTED_M5_HEADLESS_DEVICE
// Diagnostic boot: two prewarmed blocks, enough to exercise the prewarmed path (persistent map,
// suballocation, return to the pool) without displacing the memory baseline by 128 MiB. The
// production count (8) must be decided by the high-water mark of blocks in use, not by taste.
constexpr u64 PREWARM_BLOCKS = 2;
constexpr u64 BLOCK_SIZE = 8_MB;
#else
constexpr u64 PREWARM_BLOCKS = 8;
constexpr u64 BLOCK_SIZE = 16_MB;
#endif

u64 RoundAllocationSize(u64 size) {
    // Never zero: a zero-byte VkBuffer is illegal and the driver answered it with
    // VK_ERROR_INITIALIZATION_FAILED (and once with a read of -1 that killed the process).
    if (size == 0) {
        return LARGE_MIN_GRANULARITY;
    }
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* env = std::getenv("BB_STAGING_KEEP_MB");
    keep_blocks = (env ? std::strtoull(env, nullptr, 10) : 512) * 1_MB / BLOCK_SIZE;
#ifdef UNCHARTED_UWP
    {
        const auto props = instance.GetPhysicalDevice().getMemoryProperties();
        for (u32 i = 0; i < props.memoryTypeCount; ++i) {
            const auto& t = props.memoryTypes[i];
            std::printf("M5 memtype %u: flags=0x%x heap=%u heap_size=%llu MiB\n", i,
                        static_cast<unsigned>(t.propertyFlags), t.heapIndex,
                        static_cast<unsigned long long>(props.memoryHeaps[t.heapIndex].size >> 20));
        }
        std::fflush(stdout);
    }
#endif
#ifdef UNCHARTED_UWP
    {
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS]{};
        vmaGetHeapBudgets(instance.GetAllocator(), budgets);
        std::printf("M5 vma heap0: budget=%llu MiB usage=%llu MiB allocation=%llu MiB\n",
                    static_cast<unsigned long long>(budgets[0].budget >> 20),
                    static_cast<unsigned long long>(budgets[0].usage >> 20),
                    static_cast<unsigned long long>(budgets[0].statistics.blockBytes >> 20));
        std::fflush(stdout);
        /* bbport: the 16 MiB host probe that used to live here created its own VMA block, and the ICD
         * charges a 1 GiB upload heap for every host-visible device memory — a diagnostic was costing a
         * gigabyte. The pool covers this memory type now. */
    }
#endif
#ifdef UNCHARTED_UWP
    std::printf("M5 staging: block=%llu KiB prewarm=%llu\n",
                static_cast<unsigned long long>(BLOCK_SIZE >> 10),
                static_cast<unsigned long long>(PREWARM_BLOCKS));
    std::fflush(stdout);
#endif
    Ring& ring = rings[u32(MemoryType::HostUncached)];
    for (u64 i = 0; i < PREWARM_BLOCKS; ++i) {
        std::printf("M5 staging: creating block %llu\n", static_cast<unsigned long long>(i));
        std::fflush(stdout);
        auto& block = ring.blocks.emplace_back(Block{
            .buffer = std::make_unique<VideoCore::StreamBuffer>(
                instance, scheduler, MemoryType::HostUncached, BLOCK_SIZE),
            .last_used_frame = frame,
        });
#ifdef UNCHARTED_UWP
        std::printf("M5 staging: block %llu ready, mapped=%llu\n",
                    static_cast<unsigned long long>(i),
                    static_cast<unsigned long long>(block.buffer->mapped_data.size()));
        std::fflush(stdout);
#endif
        if (!block.buffer->mapped_data.empty()) {
#ifdef UNCHARTED_M5_HEADLESS_DEVICE
            // Diagnostic boot: the prewarm memset is the largest host write in the boot and a
            // prime suspect for the heap corruption seen afterwards (a printf crashed inside
            // ntdll). Skip it; no draw reads these blocks before a frame is submitted.
            if (std::getenv("BB_STAGING_CLEAR") != nullptr) {
                std::memset(block.buffer->mapped_data.data(), 0,
                            block.buffer->mapped_data.size());
            }
#else
            std::memset(block.buffer->mapped_data.data(), 0, block.buffer->mapped_data.size());
#endif
        }
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment,
                                            bool deferred) {
    Ring& ring = rings[u32(type)];
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred);
    }
    return RequestFromRing(ring, size, alignment, type);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type) {
    Block* best{};
    u64 best_offset{};
    for (size_t i = 0; i < ring.blocks.size(); ++i) {
        const size_t index = (ring.current + i) % ring.blocks.size();
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        // Cap by bytes, not by count: a block that never becomes reusable again is a leak the
        // budget would only notice much later. Eager trim first, then account the hit. The request
        // still proceeds — failing to serve it would turn a memory-pressure event into a stall.
        constexpr u64 DefaultCapBytes = 256_MB;
        static const u64 cap_bytes = [] {
            const char* env = std::getenv("BB_RING_CAP_MB");
            return env ? std::strtoull(env, nullptr, 10) << 20 : DefaultCapBytes;
        }();
        u64 held = 0;
        for (const Block& block : ring.blocks) {
            held += block.buffer->SizeBytes();
        }
        if (held + BLOCK_SIZE > cap_bytes) {
            TrimRing(ring, /*eager=*/true);
            u64 after = 0;
            for (const Block& block : ring.blocks) {
                after += block.buffer->SizeBytes();
            }
            static std::atomic<u32> cap_hits{0};
            const u32 seen = cap_hits.fetch_add(1, std::memory_order_relaxed) + 1;
            if (seen <= 8 || (seen % 100) == 0) {
                std::printf("M5 ring CAP: held=%llu MiB cap=%llu MiB after_trim=%llu MiB "
                            "blocks=%zu cap_hits=%u\n",
                            static_cast<unsigned long long>(held >> 20),
                            static_cast<unsigned long long>(cap_bytes >> 20),
                            static_cast<unsigned long long>(after >> 20), ring.blocks.size(), seen);
                std::fflush(stdout);
            }
            if (after + BLOCK_SIZE > cap_bytes) {
                // Nothing to trim: take the wait so the GPU can release what is in flight.
                const u64 tick = scheduler.CurrentTick();
                if (tick > 1) {
                    scheduler.Wait(tick - 1);
                }
            }
        }
        best = &ring.blocks.emplace_back(Block{
            .buffer =
                std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, BLOCK_SIZE),
            .last_used_frame = frame,
        });
        ring.current = ring.blocks.size() - 1;
        best_offset = *best->buffer->Reserve(size, alignment, false);
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;
    // bbport: the block is now referenced by the command buffer being recorded. The scheduler
    // stamps it with the submit's value (or releases it if that command buffer is dropped), which
    // is what makes reuse safe while several producers record.
    if (auto* scope = scheduler.ActiveScope()) {
        scope->Reference(&buffer);
    }

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            !scheduler.IsFree(entry.tick)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    // bbport: the device-local rings (detiling scratch) hold video memory: on a small card idle
    // blocks go after ~4 s instead of staying (up to 512 MiB) for 30 s.
    const bool tight = instance.GetTotalMemoryBudget() < 5_GB && !Vram::Legacy();
    for (size_t i = 0; i < rings.size(); ++i) {
        TrimRing(rings[i], tight && i == u32(MemoryType::DeviceLocal));
    }
    for (size_t i = 0; i < large_caches.size(); ++i) {
        // Large staging buffers of every memory type count against the same absolute UWP app
        // budget. Under pressure, retire any idle/free entry after ~4 s, not only device-local.
        TrimLarge(large_caches[i], tight);
    }
    PublishStats();
}

void StagingBufferPool::TrimRing(Ring& ring, bool eager) {
    auto& blocks = ring.blocks;
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        if (blocks.size() <= (eager ? 1 : keep_blocks) ||
            frame - block.last_used_frame < (eager ? 240 : RING_IDLE_FRAMES) ||
            !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache, bool eager) {
    std::erase_if(cache, [this, eager](const LargeBuffer& entry) {
        return !entry.held && frame - entry.last_used_frame >= (eager ? 240 : LARGE_IDLE_FRAMES) &&
               scheduler.IsFree(entry.tick);
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
