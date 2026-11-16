#ifdef UNCHARTED_TLS_TEST_BYTES
// TLS-limit experiment: static thread-local bytes of the same order the renderer objects add.
thread_local unsigned char bb_tls_probe[UNCHARTED_TLS_TEST_BYTES];
static int TlsProbe() {
    bb_tls_probe[0] = 0x5A;
    return bb_tls_probe[0];
}
#else
static int TlsProbe() {
    return 0;
}
#endif

#if defined(UNCHARTED_CODE_PAD_KB) && UNCHARTED_CODE_PAD_KB > 0
extern "C" int bb_code_pad_touch();
static int CodePadProbe() {
    return bb_code_pad_touch();
}
#else
static int CodePadProbe() {
    return 0;
}
#endif

#if UNCHARTED_BOOT_STAGE < 1
// Stage 0 is used to reproduce the pre-renderer link (swapchain/present gates only).
#include <cstdio>
#include <csignal>
#include <cstdlib>
extern "C" int bbgpu_uwp_renderer_boot_smoke() {
    std::printf("M5 boot: disabled (stage 0), tls probe=%d\n", TlsProbe());
    return 0;
}
#else
// UWP M5 diagnostic: builds the guest renderer objects in stages, selected at configure time by
// UNCHARTED_BOOT_STAGE (1 scheduler, 2 +runtime, 3 +liverpool, 4 +rasterizer). The stage decides
// which objects get linked, so it also bisects load-time activation failures.
#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)
#include <cstdio>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef UNCHARTED_UWP
#include <winrt/Windows.System.h>
#endif
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#if UNCHARTED_BOOT_STAGE >= 2
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/blit_helper.h"
#endif
#if UNCHARTED_BOOT_STAGE >= 3
#include "video_core/amdgpu/liverpool.h"
#endif
#if UNCHARTED_BOOT_STAGE >= 4
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#endif

extern "C" void bb_heap_check(const char* where);
extern "C" void bb_log_raw(const char* text);
extern "C" unsigned bb_oom_skip_count();

namespace {
// H2 of the over-commit question: does the process commit grow with the bytes the ICD grants? If
// it grows ~1:1 the memory is real (UMA, the console has one pool) and the refusal is the app's
// commit budget; if it stays flat the allocation is virtual and the ICD is over-committing.
unsigned long long AppMiB() {
#ifdef UNCHARTED_UWP
    try {
        return static_cast<unsigned long long>(
                   winrt::Windows::System::MemoryManager::AppMemoryUsage()) >>
               20;
    } catch (...) {
    }
#endif
    return 0;
}
} // namespace

// B2-lag (medição): is the Dozen's queue submit asynchronous? Dedicated command buffer, binary
// fence and two 1 MiB host-cached buffers — nothing of the Scheduler is touched, so this cannot
// perturb the boot path that just became deterministic. Reports the submit duration and the time
// to the first fence signal; the classification is left to the reader (log only).
void FenceCharacterization(Vulkan::Instance& renderer) {
    const char* env = std::getenv("BB_FENCE_CHAR");
    if (env == nullptr || env[0] != '1') {
        return;
    }
    const auto device = renderer.GetDevice();
    auto src = std::make_unique<VideoCore::Buffer>(renderer, 0, 1_MB,
                                                   VideoCore::MemoryType::HostCached, "fence-src");
    auto dst = std::make_unique<VideoCore::Buffer>(renderer, 0, 1_MB,
                                                   VideoCore::MemoryType::HostCached, "fence-dst");
    if (!src->Handle() || !dst->Handle()) {
        bb_log_raw("fence: buffers unavailable, skipped");
        return;
    }
    auto pool = Vulkan::Check(device.createCommandPoolUnique({
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = renderer.GetGraphicsQueueFamilyIndex(),
    }));
    const vk::CommandBufferAllocateInfo cb_info{
        .commandPool = *pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    auto cb = Vulkan::Check(device.allocateCommandBuffers(cb_info)).front();
    auto fence = Vulkan::Check(device.createFenceUnique({}));
    const vk::Fence fence_handle = *fence;
    cb.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    const vk::BufferCopy copy{.srcOffset = 0, .dstOffset = 0, .size = 1_MB};
    // The source is read by every copy, so the hazard spans the whole command buffer.
    for (u32 i = 0; i < 256; ++i) {
        cb.copyBuffer(src->Handle(), dst->Handle(), copy);
    }
    cb.end();
    Vulkan::Check(device.resetFences(std::array{fence_handle}));

    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER begin{}, after{};
    QueryPerformanceCounter(&begin);
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cb};
    const vk::Result submitted = renderer.GetGraphicsQueue().submit(std::array{submit}, fence_handle);
    QueryPerformanceCounter(&after);

    long long first_ready_us = -1;
    LARGE_INTEGER now{};
    for (;;) {
        if (device.getFenceStatus(fence_handle) == vk::Result::eSuccess) {
            QueryPerformanceCounter(&now);
            first_ready_us = (now.QuadPart - after.QuadPart) * 1'000'000 / frequency.QuadPart;
            break;
        }
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - after.QuadPart) * 1'000'000 / frequency.QuadPart > 500'000) {
            break;
        }
        SwitchToThread();
    }
    // Drain with a deadline, never an unbounded wait.
    const vk::Result waited = device.waitForFences(std::array{fence_handle}, VK_TRUE,
                                                   5'000'000'000ull);
    const long long submit_us = (after.QuadPart - begin.QuadPart) * 1'000'000 / frequency.QuadPart;
    char line[224];
    std::snprintf(line, sizeof(line),
                  "fence: submit=%lld us first_ready=%lld us result=%d wait=%d copies=256",
                  submit_us, first_ready_us, int(submitted), int(waited));
    bb_log_raw(line);
    std::printf("M5 fence characterization: submit=%lld us first_ready=%lld us wait=%d\n", submit_us,
                first_ready_us, int(waited));
    std::fflush(stdout);
}

// bbport: the same bridge, reachable from the guest path. It creates the renderer objects lazily
// (the boot smoke shows they can live in the DLL) and uploads whatever bytes it is given, so the flip
// can compare its guest snapshot with what a device buffer actually holds. Returns 1 when the hashes
// match, 0 when they differ, -1 when the renderer could not be created.
// bbport: the missing link. ProcessGraphics is the command processor for a DCB and nothing called it,
// so the guest's submissions never became GPU work. The renderer objects are created here, on demand,
// because the entry path has none (Rasterizer and Liverpool exist only in the boot smoke). The Task is
// a lazy coroutine (initial_suspend is suspend_always), so it must be resumed until done: calling it
// and dropping the return value would run nothing and look like success.
extern "C" int bbgpu_draw_submit(const void* dcb, unsigned bytes) {
    static std::unique_ptr<Vulkan::Instance> instance;
    static std::unique_ptr<Vulkan::Scheduler> scheduler;
    static std::unique_ptr<Vulkan::Runtime> runtime;
    static std::unique_ptr<AmdGpu::Liverpool> liverpool;
    static std::unique_ptr<Vulkan::Rasterizer> rasterizer;
    if (!rasterizer) {
        /* bbport: 0xE0000001 is STATUS_ASSERTION_FAILURE, raised by the CRT (assert/abort) rather than by
         * the shadPS4 macros that were instrumented. These two handlers say which of the two it is
         * before the process dies silently. */
        std::set_terminate([] {
            bb_log_raw("M5 TERMINATE: std::terminate called");
            std::abort();
        });
        ::signal(SIGABRT, [](int) {
            bb_log_raw("M5 SIGABRT: CRT assert/abort reached");
            _exit(70);
        });
        try {
            instance = std::make_unique<Vulkan::Instance>(-1, false);
            scheduler = std::make_unique<Vulkan::Scheduler>(*instance, false);
            runtime = std::make_unique<Vulkan::Runtime>(*instance, *scheduler);
            liverpool = std::make_unique<AmdGpu::Liverpool>();
            rasterizer = std::make_unique<Vulkan::Rasterizer>(*instance, *scheduler, *runtime,
                                                              liverpool.get());
        } catch (...) {
            bb_log_raw("M5 draw: renderer creation failed");
            return -1;
        }
        std::printf("M5 draw: rasterizer+liverpool ready\n");
        std::fflush(stdout);
    }
    if (!dcb || bytes == 0) return -1;
    /* bbport: process a synthetic NOP first. If even a one-word PM4 NOP asserts, the renderer setup is
     * wrong; if the NOP passes and the guest buffer asserts, the failure is content, not wiring. */
    {
        static const u32 nop_word = 0xC0001000u;  // PM4 type 3, opcode 0x10 (NOP), 1 word
        const int nop_result = liverpool->ProcessGraphicsBlocking(std::span<const u32>{&nop_word, 1});
        std::printf("M5 draw: synthetic nop result=%d\n", nop_result);
        std::fflush(stdout);
    }
    const auto* words = static_cast<const u32*>(dcb);
    std::span<const u32> dcb_span{words, bytes / 4};
    const int done = liverpool->ProcessGraphicsBlocking(dcb_span);
    static std::atomic<u32> submits{0};
    const u32 n = submits.fetch_add(1, std::memory_order_relaxed) + 1;
    const u32 steps_removed = 0;
    if (n <= 4) {
        std::printf("M5 draw: processed dcb=%p words=%u done=%d steps=%u submits=%u\n",
                    dcb, bytes / 4, done, steps_removed, n);
        std::fflush(stdout);
    }
    return done;
}

static u64 HashBytes(const void* data, size_t size);
extern "C" int bbgpu_upload_probe(const void* bytes, unsigned size) {
    static std::unique_ptr<Vulkan::Instance> instance;
    static std::unique_ptr<Vulkan::Scheduler> scheduler;
    static std::unique_ptr<Vulkan::Runtime> runtime;
    if (!instance) {
        try {
            instance = std::make_unique<Vulkan::Instance>(-1, false);
            scheduler = std::make_unique<Vulkan::Scheduler>(*instance, false);
            runtime = std::make_unique<Vulkan::Runtime>(*instance, *scheduler);
        } catch (...) {
            bb_log_raw("upload probe: renderer unavailable");
            return -1;
        }
    }
    const u64 len = size ? size : 4096;
    auto& pool = runtime->GetStagingPool();
    auto src = pool.Request(len, VideoCore::MemoryType::HostCached);
    auto rb = pool.Request(len, VideoCore::MemoryType::HostCached);
    if (src.mapped == nullptr || rb.mapped == nullptr) return -1;
    std::memcpy(src.mapped, bytes, static_cast<size_t>(len));
    std::memset(rb.mapped, 0, static_cast<size_t>(len));
    VideoCore::Buffer dst(runtime->GetInstance(), 0, len, VideoCore::MemoryType::DeviceLocal,
                          "flip-dst");
    if (!dst.Handle()) return -1;
    const auto* src_base = src.buffer->mapped_data.data();
    const auto* rb_base = rb.buffer->mapped_data.data();
    const u64 src_at = static_cast<u64>(reinterpret_cast<const u8*>(src.mapped) - src_base);
    const u64 rb_at = static_cast<u64>(reinterpret_cast<const u8*>(rb.mapped) - rb_base);
    auto pool_cmd = Vulkan::Check(runtime->GetInstance().GetDevice().createCommandPoolUnique({
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = runtime->GetInstance().GetGraphicsQueueFamilyIndex(),
    }));
    const vk::CommandBufferAllocateInfo cb_info{.commandPool = *pool_cmd,
                                                .level = vk::CommandBufferLevel::ePrimary,
                                                .commandBufferCount = 1};
    auto cb = Vulkan::Check(runtime->GetInstance().GetDevice().allocateCommandBuffers(cb_info)).front();
    auto fence = Vulkan::Check(runtime->GetInstance().GetDevice().createFenceUnique({}));
    const vk::Fence fence_handle = *fence;
    cb.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    cb.copyBuffer(src.buffer->Handle(), dst.Handle(),
                  vk::BufferCopy{.srcOffset = src_at, .dstOffset = 0, .size = len});
    cb.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {},
                       vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                         .dstAccessMask = vk::AccessFlagBits::eTransferRead},
                       {}, {});
    cb.copyBuffer(dst.Handle(), rb.buffer->Handle(),
                  vk::BufferCopy{.srcOffset = 0, .dstOffset = rb_at, .size = len});
    cb.end();
    Vulkan::Check(runtime->GetInstance().GetDevice().resetFences(std::array{fence_handle}));
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cb};
    const vk::Result r = runtime->GetInstance().GetGraphicsQueue().submit(std::array{submit}, fence_handle);
    if (r == vk::Result::eSuccess) {
        runtime->GetInstance().GetDevice().waitForFences(fence_handle, true, 5'000'000'000ull);
    }
    const u64 hs = HashBytes(src.mapped, static_cast<size_t>(len));
    const u64 hd = HashBytes(rb.mapped, static_cast<size_t>(len));
    std::printf("M5 flip verify: bytes=%llu hash_snapshot=%016llx hash_device=%016llx equal=%d\n",
                static_cast<unsigned long long>(len), static_cast<unsigned long long>(hs),
                static_cast<unsigned long long>(hd), hs == hd ? 1 : 0);
    std::fflush(stdout);
    return hs == hd ? 1 : 0;
}

// bbport: proof that the upload bridge is real, without needing a frame from the guest. A host range
// (standing in for guest memory) is copied to a device-local buffer and read back through a second
// host range; the hashes must match. This is the mechanism the first flip depends on, and it is the
// one thing about it that can be verified before any renderer exists.
static u64 HashBytes(const void* data, size_t size) {
    const auto* bytes = static_cast<const u8*>(data);
    u64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

void UploadRoundTrip(Vulkan::Runtime& runtime) {
    const char* env = std::getenv("BB_UPLOAD_TEST");
    if (env == nullptr || env[0] != '1') {
        return;
    }
    const auto device = runtime.GetInstance().GetDevice();
    auto& scheduler = runtime.GetScheduler();
    auto& pool = runtime.GetStagingPool();
    constexpr u64 Size = 1_MB;

    /* bbport: HostCached first. The half-copy (first 512 KiB wrong, last 512 KiB right) is the
     * signature of write-combined staging without a flush; a coherent block rules that out with one
     * run, without guessing at flush APIs the pool does not expose. */
    auto src = pool.Request(Size, VideoCore::MemoryType::HostCached);
    auto rb = pool.Request(Size, VideoCore::MemoryType::HostCached);
    VideoCore::Buffer dst(runtime.GetInstance(), 0, Size, VideoCore::MemoryType::DeviceLocal,
                          "upload-dst");
    if (src.mapped == nullptr || rb.mapped == nullptr || !dst.Handle()) {
        bb_log_raw("upload: resources unavailable, skipped");
        return;
    }
    auto* pattern = static_cast<u8*>(src.mapped);
    /* bbport: a cheap LCG, not a repeating expression. The previous pattern had a 256 KiB period,
     * so every quarter of the buffer hashed identically and the comparison could not tell a shifted
     * copy from a correct one. */
    u32 state = 0x12345678u;
    for (u64 i = 0; i < Size; ++i) {
        state = state * 1664525u + 1013904223u;
        pattern[i] = static_cast<u8>(state >> 24);
    }
    std::memset(rb.mapped, 0, static_cast<size_t>(Size));

    auto pool_cmd = Vulkan::Check(device.createCommandPoolUnique({
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = runtime.GetInstance().GetGraphicsQueueFamilyIndex(),
    }));
    const vk::CommandBufferAllocateInfo cb_info{
        .commandPool = *pool_cmd,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    };
    auto cb = Vulkan::Check(device.allocateCommandBuffers(cb_info)).front();
    auto fence = Vulkan::Check(device.createFenceUnique({}));
    const vk::Fence fence_handle = *fence;
    /* bbport: derive the copy offsets from the mapped pointers instead of trusting ref.offset.
     * If mapped is already offset-adjusted, ref.offset would be applied twice and the readback would
     * land somewhere else - which is exactly the half-copy seen in the logs. */
    const auto* src_base = src.buffer->mapped_data.data();
    const auto* rb_base = rb.buffer->mapped_data.data();
    const u64 src_at = static_cast<u64>(reinterpret_cast<const u8*>(src.mapped) - src_base);
    const u64 rb_at = static_cast<u64>(reinterpret_cast<const u8*>(rb.mapped) - rb_base);
    std::printf("M5 upload device buffer: bytes=%llu\n", (unsigned long long)dst.SizeBytes());
    std::printf("M5 upload refs: src_base=%p src_map=%p src_off=%llu src_at=%llu "
                "rb_base=%p rb_map=%p rb_off=%llu rb_at=%llu\n",
                (const void*)src_base, (const void*)src.mapped, (unsigned long long)src.offset,
                (unsigned long long)src_at, (const void*)rb_base, (const void*)rb.mapped,
                (unsigned long long)rb.offset, (unsigned long long)rb_at);
    std::fflush(stdout);
    cb.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    cb.copyBuffer(src.buffer->Handle(), dst.Handle(),
                  vk::BufferCopy{.srcOffset = src_at, .dstOffset = 0, .size = Size});
    /* bbport: Vulkan has no implicit memory dependency between two transfers, so the read-back can
     * observe the device buffer half written - which is exactly the clean 512 KiB split seen in the
     * logs (SubmitSuccess and the fence say nothing about it). */
    cb.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                       vk::PipelineStageFlagBits::eTransfer, {},
                       vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                         .dstAccessMask = vk::AccessFlagBits::eTransferRead},
                       {}, {});
    cb.copyBuffer(dst.Handle(), rb.buffer->Handle(),
                  vk::BufferCopy{.srcOffset = 0, .dstOffset = rb_at, .size = Size});
    cb.end();
    Vulkan::Check(device.resetFences(std::array{fence_handle}));
    const vk::SubmitInfo submit{.commandBufferCount = 1, .pCommandBuffers = &cb};
    const vk::Result result = runtime.GetInstance().GetGraphicsQueue().submit(
        std::array{submit}, fence_handle);
    const vk::Result waited = result == vk::Result::eSuccess
                                  ? device.waitForFences(fence_handle, true, 5'000'000'000ull)
                                  : result;
    const u64 src_hash = HashBytes(src.mapped, Size);
    const u64 dst_hash = HashBytes(rb.mapped, Size);
    const auto* s8 = static_cast<const u8*>(src.mapped);
    const auto* r8 = static_cast<const u8*>(rb.mapped);
    std::printf("M5 upload offsets: src=%llu rb=%llu src8=%02x%02x%02x%02x%02x%02x%02x%02x " 
                "dst8=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                static_cast<unsigned long long>(src.offset),
                static_cast<unsigned long long>(rb.offset),
                s8[0],s8[1],s8[2],s8[3],s8[4],s8[5],s8[6],s8[7],
                r8[0],r8[1],r8[2],r8[3],r8[4],r8[5],r8[6],r8[7]);
    std::fflush(stdout);
    {
        constexpr u64 Quarter = Size / 4;
        std::printf("M5 upload quarters:");
        for (u32 q = 0; q < 4; ++q) {
            std::printf(" q%u=%016llx/%016llx", q,
                        static_cast<unsigned long long>(HashBytes(s8 + q * Quarter, Quarter)),
                        static_cast<unsigned long long>(HashBytes(r8 + q * Quarter, Quarter)));
        }
        std::printf("\n");
        std::fflush(stdout);
    }
    std::printf("M5 upload roundtrip: size=%llu submit=%s wait=%s hash_src=%016llx "
                "hash_dst=%016llx equal=%d\n",
                static_cast<unsigned long long>(Size), vk::to_string(result).c_str(),
                vk::to_string(waited).c_str(), static_cast<unsigned long long>(src_hash),
                static_cast<unsigned long long>(dst_hash), src_hash == dst_hash ? 1 : 0);
    std::fflush(stdout);

}


// block stays referenced until the command buffer that uses it is submitted, so the churn submits
// every few cycles: that is what releases them, and it is the same path the game uses.
void StagingChurn(Vulkan::Runtime& runtime, Vulkan::Scheduler& scheduler) {
    const char* env = std::getenv("BB_STAGING_CHURN");
    if (env == nullptr) {
        return;
    }
    const int cycles = std::atoi(env);
    if (cycles <= 0) {
        return;
    }
    auto& pool = runtime.GetStagingPool();
    char line[192];
    const unsigned long long app_begin = AppMiB();
    std::snprintf(line, sizeof(line), "churn: start cycles=%d blocks=%zu app=%llu MiB", cycles,
                  pool.HostUncachedBlockCount(), app_begin);
    bb_log_raw(line);
    for (int i = 0; i < cycles; ++i) {
        auto ref = pool.Request(1ull << 20, VideoCore::MemoryType::HostUncached);
        if (ref.mapped != nullptr) {
            std::memset(ref.mapped, 0, static_cast<size_t>(ref.size));
        }
        // Release the references every 8 cycles: the command buffer holding them reaches the GPU,
        // its value completes, and the blocks become reusable again.
        if ((i % 8) == 7) {
            Vulkan::SubmitInfo info{};
            scheduler.Flush(info);
            scheduler.Wait(scheduler.CurrentTick() > 0 ? scheduler.CurrentTick() - 1 : 0);
        }
        pool.TickFrame();
        if ((i % 16) == 0 || i == cycles - 1) {
            std::snprintf(line, sizeof(line), "churn: cycle %d blocks=%zu app=%llu MiB", i,
                          pool.HostUncachedBlockCount(), AppMiB());
            bb_log_raw(line);
        }
    }
    const unsigned long long app_end = AppMiB();
    std::snprintf(line, sizeof(line), "churn: end blocks=%zu app=%llu MiB delta=%lld MiB",
                  pool.HostUncachedBlockCount(), app_end,
                  static_cast<long long>(app_end) - static_cast<long long>(app_begin));
    bb_log_raw(line);
    std::printf("M5 staging churn: cycles=%d blocks=%zu app %llu -> %llu MiB\n", cycles,
                pool.HostUncachedBlockCount(), app_begin, app_end);
    std::fflush(stdout);
}

// B1 closing test: walk the allocator up with 64 MiB device-local buffers (the same path the game
// uses), stop at the first refusal, release everything and prove the device still allocates with a
// 64 KiB canary. Every step writes its intention and result to the raw channel first: if the ICD
// kills the process instead of returning an error, the last intention is the boundary.
void OomRamp(Vulkan::Instance& renderer) {
    const char* mode = std::getenv("BB_OOM_INJECT");
    if (mode == nullptr || std::strcmp(mode, "ramp") != 0) {
        return;
    }
    constexpr u64 Step = 64ull << 20;
    // The Dozen over-commits: 40 steps (2560 MiB) were granted against a heap reported as 1280 MiB
    // with zero refusals, so the walk has to be long enough to reach the real limit — the app's
    // commit budget — and say which one it was.
    u32 max_steps = 120;
    if (const char* env = std::getenv("BB_OOM_STEPS")) {
        const int value = std::atoi(env);
        if (value > 0 && value <= 400) {
            max_steps = static_cast<u32>(value);
        }
    }
    std::vector<std::unique_ptr<VideoCore::Buffer>> held;
    u64 granted = 0;
    u32 steps = 0;
    char line[192];
    for (; steps < max_steps; ++steps) {
        std::snprintf(line, sizeof(line),
                      "OOM ramp: step %u request=64 MiB granted=%llu MiB", steps, granted >> 20);
        bb_log_raw(line);
        auto buffer = std::make_unique<VideoCore::Buffer>(renderer, 0, Step,
                                                         VideoCore::MemoryType::DeviceLocal,
                                                         "oom-inject");
        if (!buffer->Handle()) {
            std::snprintf(line, sizeof(line),
                          "OOM ramp: refused at step %u after %llu MiB app=%llu MiB", steps,
                          granted >> 20, AppMiB());
            bb_log_raw(line);
            break;
        }
        granted += Step;
        held.push_back(std::move(buffer));
        std::snprintf(line, sizeof(line), "OOM ramp: step %u ok granted=%llu MiB app=%llu MiB",
                      steps, granted >> 20, AppMiB());
        bb_log_raw(line);
    }
    held.clear();
    bb_log_raw("OOM ramp: released all");    auto canary = std::make_unique<VideoCore::Buffer>(renderer, 0, 64ull << 10,
                                                     VideoCore::MemoryType::DeviceLocal,
                                                     "oom-canary");
    std::snprintf(line, sizeof(line), "OOM ramp: canary %s skips=%u",
                  canary->Handle() ? "ok" : "FAILED", bb_oom_skip_count());
    bb_log_raw(line);
    std::printf("M5 oom ramp: steps=%u granted=%llu MiB skips=%u canary=%s capped=%d\n", steps,
                granted >> 20, bb_oom_skip_count(), canary->Handle() ? "ok" : "FAILED",
                steps == max_steps ? 1 : 0);
    std::fflush(stdout);
}

namespace {
// App memory at each stage: the renderer objects allocate GPU/committed memory, and a crash
// inside a driver call must be distinguishable from running out of the 5120 MiB app budget.
// WinRT is only queried best-effort: a throw here must never take the diagnostic run down.
void Mem(const char* stage) {
#ifdef UNCHARTED_UWP
    unsigned long long mib = 0;
    bool ok = false;
    try {
        mib = static_cast<unsigned long long>(
                  winrt::Windows::System::MemoryManager::AppMemoryUsage()) >> 20;
        ok = true;
    } catch (...) {
    }
    if (ok) {
        std::printf("M5 mem: %s app=%llu MiB\n", stage, mib);
    } else {
        std::printf("M5 mem: %s unavailable\n", stage);
    }
    std::fflush(stdout);
    bb_heap_check(stage);
#else
    (void)stage;
#endif
}

// Host-provided Vulkan instance. Creating one inside the DLL fails in
// enumeratePhysicalDevices because the ICD is bound to the module that loaded it, so the host
// creates the instance and hands the object over before this runs.
void* g_host_instance = nullptr;
} // namespace

extern "C" void bb_renderer_set_host_instance(void* instance) {
    g_host_instance = instance;
}

namespace {
int Run() {
    bb_heap_check("boot start");
    std::unique_ptr<Vulkan::Instance> owned_instance;
    Vulkan::Instance* renderer_ptr = static_cast<Vulkan::Instance*>(g_host_instance);
    if (renderer_ptr == nullptr) {
        owned_instance = std::make_unique<Vulkan::Instance>(-1, false);
        renderer_ptr = owned_instance.get();
    }
    Vulkan::Instance& renderer = *renderer_ptr;
    std::printf("M5 boot (stage %d): instance ready\n", UNCHARTED_BOOT_STAGE);
    std::fflush(stdout);
    Mem("instance");

#if UNCHARTED_BOOT_TOUCH_ONLY
    // Link-only variant: pulls the Scheduler constructor and everything it needs into the image
    // without ever executing it, separating load-time failures from constructor failures.
    ([&renderer] {
        auto never_constructed = std::make_unique<Vulkan::Scheduler>(renderer, false);
        (void)never_constructed;
    });
#else
    std::puts("M5 boot: constructing scheduler"); std::fflush(stdout);
    // Every renderer object lives on the heap, never in this frame: sizeof(Vulkan::Rasterizer) is
    // in the hundreds of megabytes (its std::array<TextureSet, 32768> alone), so as a local it
    // made the boot smoke's prologue call _alloca_probe, which walked down from a 16 MiB thread
    // stack until it hit an unmapped page (write to 0x44fffff000 in one run) or, when the pages
    // below happened to be mapped, let a giant frame overlap the heap and corrupt it. That is why
    // the boot died at a different place on every run.
    auto scheduler = std::make_unique<Vulkan::Scheduler>(renderer, false);
#endif
    std::printf("M5 boot (stage %d): scheduler ready\n", UNCHARTED_BOOT_STAGE);
    std::fflush(stdout);
    Mem("scheduler");
#if UNCHARTED_BOOT_STAGE >= 2
    auto runtime = std::make_unique<Vulkan::Runtime>(renderer, *scheduler);
    std::printf("M5 boot (stage %d): runtime ready\n", UNCHARTED_BOOT_STAGE);
    std::fflush(stdout);
    Mem("runtime");
#endif
#if UNCHARTED_BOOT_STAGE >= 3
    AmdGpu::Liverpool liverpool{};
    std::printf("M5 boot (stage %d): liverpool ready\n", UNCHARTED_BOOT_STAGE);
    std::fflush(stdout);
    Mem("liverpool");
#endif
#if UNCHARTED_BOOT_STAGE >= 4
    std::puts("M5 boot: constructing rasterizer"); std::fflush(stdout);
    Mem("rasterizer-before");
    auto rasterizer =
        std::make_unique<Vulkan::Rasterizer>(renderer, *scheduler, *runtime, &liverpool);
    std::printf("M5 boot (stage %d): rasterizer ready\n", UNCHARTED_BOOT_STAGE);
    std::fflush(stdout);
    Mem("rasterizer");
#endif
    std::printf("M5 boot (stage %d): code pad probe=%d\n", UNCHARTED_BOOT_STAGE, CodePadProbe());
    std::printf("M5 boot PASS (stage %d): guest renderer objects constructed; no draw issued\n",
                UNCHARTED_BOOT_STAGE);
    std::fflush(stdout);
#if UNCHARTED_BOOT_STAGE >= 4
    OomRamp(renderer);
    FenceCharacterization(renderer);
    StagingChurn(*runtime, *scheduler);
    UploadRoundTrip(*runtime);
#endif
    return 0;
}
} // namespace

extern "C" int bbgpu_uwp_renderer_boot_smoke() {
    bb_log_raw("R1 smoke-enter");
    try {
        const int result = Run();
        bb_log_raw("R2 smoke-returned");
        return result;
    } catch (const std::exception& error) {
        std::printf("M5 boot FAILED (stage %d): %s\n", UNCHARTED_BOOT_STAGE, error.what());
        std::fflush(stdout);
        return 1;
    } catch (...) {
        std::printf("M5 boot FAILED (stage %d): exception type is not std::exception (thread %lu)\n",
                    UNCHARTED_BOOT_STAGE, GetCurrentThreadId());
        std::fflush(stdout);
        return 2;
    }
}
#endif
#endif
