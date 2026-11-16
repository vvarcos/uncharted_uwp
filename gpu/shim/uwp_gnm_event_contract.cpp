extern "C" void bb_log_raw(const char* text);
extern "C" int bbgpu_draw_submit(const void*, unsigned);
extern "C" unsigned bbgpu_uwp_videoout_signal_flip(unsigned long long);
extern "C" unsigned bbgpu_uwp_videoout_flip_queue_count(void);
extern "C" int bbgpu_upload_probe(const void*, unsigned);
#include "core/memory.h"
// UWP-only extraction of GNM's real equeue/IRQ registration contract.
// No GPU is initialized and guest rendering/IRQ generation is NOT simulated.
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <set>
#include <utility>
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/libs.h"
#include "core/platform.h"

namespace Libraries::Kernel {
int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue*,const char*);
int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue);
int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue,OrbisKernelEvent*,int,int*,OrbisKernelUseconds*);
}

namespace {
using namespace Libraries::Kernel;
std::mutex gnm_lock;
std::set<std::pair<OrbisKernelEqueue,u64>> registered;
constexpr u64 GfxEop=static_cast<u64>(Platform::InterruptId::GfxEop);

// sceGnmSubmitAndFlipCommandBuffers: the combined submit+flip the guest uses for presents. There is
// no GPU submission path or renderer attached yet, so this validates what the guest asked for,
// records it, and returns success WITHOUT emitting a flip event: the event belongs to the vblank
// that completes this flip, and there is no PresentClock driving it yet. Logging the arguments is
// what turns the first real flip into evidence instead of a guess.
extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
// bbport: the flip request is a record, not a callback. The guest thread only copies its DCB and
// publishes an index: no lock, no allocation, no submit, no wait. The renderer drains the ring in its
// own frame, with its own locks, on its own thread - which is also how the hardware behaves (the flip
// completes on the vblank, so the guest has nothing to wait for). The snapshot is mandatory: the guest
// rewrites the DCB next frame, so the bytes to compare must be captured here.
struct FlipRequest {
    u32 handle;
    u32 buffer_index;
    u32 flip_mode;
    u64 dcb_guest;
    u64 dcb_size;
    u64 hash_at_enqueue;
    u8 dcb_snapshot[4096];
};
static constexpr u32 kFlipRingSize = 8;
static FlipRequest flip_ring[kFlipRingSize];
static std::atomic<u32> flip_produced{0};
static std::atomic<u32> flip_consumed{0};
static std::atomic<u32> flip_dropped{0};

// Called by the renderer when it drains. Returns false when there is nothing to consume.
extern "C" int bbgpu_flip_take_request(FlipRequest* out) {
    const u32 produced = flip_produced.load(std::memory_order_acquire);
    u32 consumed = flip_consumed.load(std::memory_order_relaxed);
    if (consumed == produced) return 0;
    if (out) *out = flip_ring[consumed % kFlipRingSize];
    flip_consumed.store(consumed + 1, std::memory_order_release);
    return 1;
}
extern "C" unsigned bbgpu_flip_dropped_count(void) { return flip_dropped.load(std::memory_order_relaxed); }
extern "C" unsigned bbgpu_flip_pending_count(void) {
    return flip_produced.load(std::memory_order_acquire) - flip_consumed.load(std::memory_order_acquire);
}

extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
s32 PS4_SYSV_ABI SubmitAndFlipCommandBuffers(u32 count, const void* const* dcb, const void* const* ccb,
                                            const u64* dcb_sizes, const u32* ccb_sizes,
                                            u32 buffer_index, u32 flip_mode, u64 flip_arg) {
    const void* d0 = (count>0 && dcb) ? dcb[0] : nullptr;
    const int mapped = d0 ? runtime_memory_is_mapped((uintptr_t)d0, 4096) : -1;
    if (d0 && mapped > 0) {
        const u32 slot = flip_produced.load(std::memory_order_relaxed);
        FlipRequest& req = flip_ring[slot % kFlipRingSize];
        const auto* bytes = static_cast<const u8*>(d0);
        /* Copy first, then compute the hash over the copy: the fingerprint must describe exactly the
         * bytes the renderer will upload, not the guest buffer as it was a moment earlier. */
        for (u32 k = 0; k < 4096; ++k) req.dcb_snapshot[k] = bytes[k];
        u64 h = 0xcbf29ce484222325ull;
        for (u32 k = 0; k < 4096; ++k) { h ^= req.dcb_snapshot[k]; h *= 0x100000001b3ull; }
        req.handle = 0;
        req.buffer_index = buffer_index;
        req.flip_mode = flip_mode;
        req.dcb_guest = (u64)(uintptr_t)d0;
        req.dcb_size = 4096;
        req.hash_at_enqueue = h;
        flip_produced.store(slot + 1, std::memory_order_release);
        /* PresentClock, VIRTUAL mode: one tick per flip, which is the quiescent point this build has.
         * The event is emitted ONLY for a registered equeue; with none registered the flip still
         * completes on its tick and is counted, because inventing an ident would wake the GPU thread
         * with a value nobody registered. */
        static std::atomic<u64> vblank_count{0};
        static std::atomic<u32> flip_completed_no_registration{0};
        const u64 tick = vblank_count.fetch_add(1, std::memory_order_relaxed) + 1;
        const unsigned queues = bbgpu_uwp_videoout_flip_queue_count();
        if (queues == 0) {
            flip_completed_no_registration.fetch_add(1, std::memory_order_relaxed);
        } else {
                bbgpu_uwp_videoout_signal_flip(tick);
        }
        static std::atomic<u32> clock_logs{0};
        if (clock_logs.fetch_add(1, std::memory_order_relaxed) < 4) {
            std::printf("M5 present clock: vblank=%llu flip=#%u registered_queues=%u "
                        "no_registration=%u\n",
                        (unsigned long long)tick, flip_produced.load(std::memory_order_relaxed),
                        queues,
                        flip_completed_no_registration.load(std::memory_order_relaxed));
            std::fflush(stdout);
        }
        /* Diagnostic only (BB_FLIP_VERIFY=1): this call creates the renderer and waits for a GPU copy,
         * so it blocks the calling thread by design. The default path publishes the index and returns. */
        if (std::getenv("BB_FLIP_VERIFY") != nullptr) {
            bbgpu_upload_probe(req.dcb_snapshot, 4096);
        }
        static std::atomic<u32> calls{0};
        const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
        if(seen<=4) {
            std::printf("M5 flip request: #%u slot=%u buffer=%u mode=%u dcb=%p bytes=4096 "
                        "rasterizer=%p "
                        "hash_snapshot=%016llx\n",
                        seen,slot%kFlipRingSize,buffer_index,flip_mode,d0,
                        (void*)Core::Memory::Instance()->GetRasterizer(),
                        (unsigned long long)h);
            std::fflush(stdout);
        }
    } else {
        flip_dropped.fetch_add(1,std::memory_order_relaxed);
    }
    /* bbport: risk gate for the draw path. Bringing the renderer into the entry path is the step that
     * spends memory (the Rasterizer alone is hundreds of MiB), so it is opt-in and the next memory
     * sample in the log shows what it cost. */
    if (std::getenv("BB_DRAW") != nullptr && d0) {
        static std::atomic<u32> draw_probes{0};
        if (draw_probes.fetch_add(1, std::memory_order_relaxed) == 0) {
            /* One DCB only: the command processor has never seen a real Uncharted buffer, so the first
             * call is the experiment. The size comes from the guest's own size table when present. */
            const u32 words = (dcb_sizes && dcb_sizes[0]) ? (u32)dcb_sizes[0] : 16384u;
            const int result = bbgpu_draw_submit(d0, words * 4u);
            std::printf("M5 draw: first dcb submit result=%d words=%u\n", result, words);
            std::fflush(stdout);
        }
    }
    return 0;
}
s32 PS4_SYSV_ABI InsertWaitFlipDone(u32 dcb,u32 bufferIndex) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=4) {
        std::printf("M5 GNM wait flip done: #%u dcb=%u buffer=%u; nothing in flight, returning\n",
                    seen,dcb,bufferIndex);
        std::fflush(stdout);
    }
    return 0;
}

s32 PS4_SYSV_ABI SubmitDone() {
    static std::atomic<u32> done_count{0};
    const u32 seen=done_count.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=8 || (seen%100)==0) {
        std::printf("M5 GNM submit done: #%u (nothing in flight; the DCB path has not been used yet)\n",seen);
        std::fflush(stdout);
    }
    return 0;
}

s32 PS4_SYSV_ABI AddEqEvent(OrbisKernelEqueue eq,u64 id,void* udata) {
    std::scoped_lock lock{gnm_lock};
    auto* equeue=GetEqueue(eq);
    if(!equeue) return ORBIS_KERNEL_ERROR_EBADF;
    // The original controller asserts above its maximum interrupt ID. Reject explicitly
    // in the diagnostic mode instead of crashing a guest thread on unsupported input.
    if(id>GfxEop) return ORBIS_KERNEL_ERROR_EINVAL;
    EqueueEvent event{};
    event.event.ident=id;
    event.event.filter=OrbisKernelEvent::Filter::GraphicsCore;
    event.event.flags=OrbisKernelEvent::Flags::Add;
    event.event.data=id;
    event.event.udata=udata;
    if(!equeue->AddEvent(event)) return ORBIS_KERNEL_ERROR_EINVAL;
    if(registered.insert({eq,id}).second) {
        Platform::IrqC::Instance()->Register(static_cast<Platform::InterruptId>(id),
            [equeue,id](Platform::InterruptId irq) {
                const auto actual=static_cast<u64>(irq);
                if(actual!=id || !(actual<=6 || actual==GfxEop)) return;
                equeue->TriggerEvent(id,OrbisKernelEvent::Filter::GraphicsCore,
                                     reinterpret_cast<void*>(static_cast<uintptr_t>(id)));
            },equeue);
    }
    std::printf("M5 GNM equeue IRQ registered: eq=0x%llx id=0x%llx; GPU not initialized\n",
                static_cast<unsigned long long>(eq),static_cast<unsigned long long>(id));
    return 0;
}
s32 PS4_SYSV_ABI DeleteEqEvent(OrbisKernelEqueue eq,u64 id) {
    std::scoped_lock lock{gnm_lock};
    auto* equeue=GetEqueue(eq);
    if(!equeue) return ORBIS_KERNEL_ERROR_EBADF;
    equeue->RemoveEvent(id,OrbisKernelEvent::Filter::GraphicsCore);
    if(registered.erase({eq,id}))
        Platform::IrqC::Instance()->Unregister(static_cast<Platform::InterruptId>(id),equeue);
    return 0;
}
}

extern "C" void bbgpu_uwp_gnm_event_cleanup(Libraries::Kernel::OrbisKernelEqueue eq) {
    std::scoped_lock lock{gnm_lock};
    auto* equeue=Libraries::Kernel::GetEqueue(eq);
    if(!equeue) return;
    for(auto it=registered.begin();it!=registered.end();) {
        if(it->first!=eq) { ++it; continue; }
        Platform::IrqC::Instance()->Unregister(static_cast<Platform::InterruptId>(it->second),equeue);
        it=registered.erase(it);
    }
}

// sceGnmMapComputeQueue: the guest hands the driver a ring buffer for a compute queue. There is no
// compute submission path in this port yet, so the mapping is acknowledged (the ring is recorded, not
// consumed) rather than refused: refusing stops video initialisation before any frame exists.
s32 PS4_SYSV_ABI MapComputeQueue(u32 pipeId, u32 queueId, u32 ringSize, void* ringBase, u64 ringOffset) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=4) {
        std::printf("M5 GNM map compute queue: #%u pipe=%u queue=%u ring=%u base=%p offset=0x%llx (recorded)\n",
                    seen,pipeId,queueId,ringSize,ringBase,static_cast<unsigned long long>(ringOffset));
        std::fflush(stdout);
    }
    return 0;
}

// sceGnmDrawInitDefaultHardwareState200: fills the caller's command buffer with the driver's
// default hardware state. There is no command processor behind these DCBs yet, so the buffer is
// acknowledged and left for the submit path (sceGnmSubmitAndFlipCommandBuffers) to record. Writing
// invented state into it would make the guest read back values no renderer produced.
void* PS4_SYSV_ABI DrawInitDefaultHardwareState200(void* dcb, u32 size) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=4) {
        std::printf("M5 GNM draw init default state 200: #%u dcb=%p size=%u (recorded)\n",
                    seen,dcb,size);
        std::fflush(stdout);
    }
    /* bbport: these GNM entry points return the advanced DCB pointer, not a status. Returning 0 made a
     * caller that writes dcb = sceGnm...() restart the stream at offset 0. Nothing is written here, so
     * the advanced pointer is the input pointer. */
    return dcb;
}

// sceGnmInsertPushMarker: a debug marker in the draw command buffer. Cosmetic by definition: the
// name is copied into the DCB for profilers, so acknowledging it changes nothing the guest can read
// back as state.
s32 PS4_SYSV_ABI InsertPopMarker(void* dcb) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=4) { std::printf("M5 GNM pop marker: #%u dcb=%p (recorded)\n",seen,dcb); std::fflush(stdout); }
    return 0;
}
s32 PS4_SYSV_ABI InsertPushMarker(void* dcb, const char* name) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=4) {
        /* bbport: the name pointer is guest memory and is not always valid here (0xd was tried once); log
         * the pointer, never dereference it from the driver side. */
        std::printf("M5 GNM push marker: #%u dcb=%p name=%p (recorded)\n",seen,dcb,(void*)name);
        std::fflush(stdout);
    }
    return 0;
}

// sceGnmSetCsShader and its draw-pipeline siblings: the guest records a shader into its DCB. The
// shader object is guest memory; only its address is logged, never dereferenced. No state is invented
// - the DCB is acknowledged so the guest's own command stream stays authoritative.
s32 PS4_SYSV_ABI SetShaderSlot(const char* slot, void* dcb, const void* shader, u32 extra) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=6) {
        std::printf("M5 GNM set %s shader: #%u dcb=%p shader=%p extra=%u (recorded)\n",
                    slot,seen,dcb,shader,extra);
        std::fflush(stdout);
    }
    return 0;
}
void* PS4_SYSV_ABI SetCsShader(void* dcb, const void* shader, u32 extra) { SetShaderSlot("cs",dcb,shader,extra); return dcb; }
void* PS4_SYSV_ABI SetVsShader(void* dcb, const void* shader, u32 extra) { SetShaderSlot("vs",dcb,shader,extra); return dcb; }
void* PS4_SYSV_ABI UpdatePsShader(void* dcb, const void* shader, u32 extra) { SetShaderSlot("update-ps",dcb,shader,extra); return dcb; }
void* PS4_SYSV_ABI UpdateVsShader(void* dcb, const void* shader, u32 extra) { SetShaderSlot("update-vs",dcb,shader,extra); return dcb; }
void* PS4_SYSV_ABI SetPsShader(void* dcb, const void* shader, u32 extra) { SetShaderSlot("ps",dcb,shader,extra); return dcb; }

// sceGnmIsUserPaEnabled: asks whether performance analysis (profiling counters) is on. Nothing in
// this port collects them, so the honest answer is 0 - disabled - and the guest takes the normal path.
s32 PS4_SYSV_ABI IsUserPaEnabled(void) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=2) { std::printf("M5 GNM user PA enabled: #%u -> 0 (no profiling in this build)\n",seen); std::fflush(stdout); }
    return 0;
}

// sceGnmAreSubmitsAllowed: asks the driver whether another submission may be queued (the guest uses
// it for back-pressure). Nothing is in flight here, so the honest answer is 1 - allowed.
s32 PS4_SYSV_ABI AreSubmitsAllowed(void) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=4) { std::printf("M5 GNM submits allowed: #%u -> 1\n",seen); std::fflush(stdout); }
    return 1;
}

// sceGnmSubmitCommandBuffers: the guest submits one or more DCBs. There is no command processor
// behind them yet, so nothing reaches the GPU: the call validates what it was given (count and the
// first buffer/size, which is what the R2 proof needs) and acknowledges. sceGnmSubmitDone completes
// the cycle; faking GPU work here would make the flip that follows describe work that never ran.
extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);
s32 PS4_SYSV_ABI SubmitCommandBuffers(u32 count, const void* const* dcb, const u32* dcb_size) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=4) {
        const void* first = (count > 0 && dcb) ? dcb[0] : nullptr;
        const u32 first_size = (count > 0 && dcb_size) ? dcb_size[0] : 0;
        const int mapped = first ? runtime_memory_is_mapped((uintptr_t)first, first_size ? first_size : 1) : -1;
        std::printf("M5 GNM submit command buffers: #%u count=%u dcb0=%p size0=%u mapped=%d\n",
                    seen,count,first,first_size,mapped);
        std::fflush(stdout);
    }
    return 0;
}

// bbport: the stubs previously registered here OVERWROTE the real GnmDriver implementations that
// bbgpu.cpp already installs via Libraries::GnmDriver::RegisterLib (gnmdriver.cpp has LIB_FUNCTION
// entries for these very NIDs, including DrawInitDefaultHardwareState200, SubmitCommandBuffers and
// SubmitAndFlipCommandBuffers). The real ones write InitSequence200 into the DCB and return
// HwInitPacketSize; the stubs wrote nothing, which is why ProcessGraphics asserted on state.
extern "C" void bbgpu_uwp_gnm_event_register(Core::Loader::SymbolsResolver* sym) {
    /* bbport: with BB_REAL_GNM=1 the driver registers into the same table first, so none of these
     * four NIDs may be added again: the resolver returns the first match, so a stub here would
     * shadow the driver and reintroduce exactly the no-op DCB that made ProcessGraphics assert. */
    static const bool real_driver = [] {
        const char* env = std::getenv("BB_REAL_GNM");
        return env && env[0] == '1';
    }();
    if (real_driver) return;


    sym->AddSymbol("jg33rEKLfVs","libSceGnmDriver","libSceGnmDriver",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(IsUserPaEnabled));





    // bbport: sceGnmSubmitDone has no arguments — it finishes the submission built by
    // sceGnmSubmitCommandBuffers, which the guest has not called yet. Acknowledging it without any
    // pending submission is honest (nothing was submitted, nothing completes); emitting a flip
    // event here would be a lie, because the event belongs to sceVideoOutSubmitFlip.
    sym->AddSymbol("yvZ73uQUqrk","libSceGnmDriver","libSceGnmDriver",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(SubmitDone));
    sym->AddSymbol("b0xyllnVY-I","libSceGnmDriver","libSceGnmDriver",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(AddEqEvent));
    sym->AddSymbol("PVT+fuoS9gU","libSceGnmDriver","libSceGnmDriver",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(DeleteEqEvent));
}

extern "C" int bbgpu_uwp_gnm_event_selftest(void) {
    using namespace Libraries::Kernel;
    OrbisKernelEqueue eq=0;
    if(sceKernelCreateEqueue(&eq,"M5-gnm-equeue")) return 1;
    if(AddEqEvent(eq,GfxEop,reinterpret_cast<void*>(uintptr_t(0x4567)))) return 2;
    if(!GetEqueue(eq)->EventExists(GfxEop,OrbisKernelEvent::Filter::GraphicsCore)) return 3;
    // Explicit self-test injection only; there is no artificial GPU IRQ during guest entry.
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
    OrbisKernelEvent event{};
    int count=0;
    OrbisKernelUseconds timeout=100000;
    if(sceKernelWaitEqueue(eq,&event,1,&count,&timeout) || count!=1 ||
       event.ident!=GfxEop || event.filter!=OrbisKernelEvent::Filter::GraphicsCore ||
       event.data!=GfxEop || event.udata!=reinterpret_cast<void*>(uintptr_t(0x4567))) return 4;
    if(DeleteEqEvent(eq,GfxEop)) return 5;
    if(GetEqueue(eq)->EventExists(GfxEop,OrbisKernelEvent::Filter::GraphicsCore)) return 6;
    if(sceKernelDeleteEqueue(eq)) return 7;
    if(AddEqEvent(eq,GfxEop,nullptr)!=ORBIS_KERNEL_ERROR_EBADF) return 8;
    OrbisKernelEqueue second=0;
    if(sceKernelCreateEqueue(&second,"M5-gnm-auto-cleanup") || AddEqEvent(second,GfxEop,nullptr)) return 9;
    if(sceKernelDeleteEqueue(second)) return 10;
    {
        std::scoped_lock lock{gnm_lock};
        if(registered.contains({second,GfxEop})) return 11;
    }
    // A signal after deletion must not reach the freed EqueueInternal object.
    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop);
    std::puts("PASS: real GNM GraphicsCore equeue/IRQ callback, explicit/queue-delete cleanup (self-test IRQ only); guest GPU disabled");
    return 0;
}
#endif

// bbport: the assert text never reached the console because stderr is not a valid channel in the UWP
// AppContainer. Write the location through the raw channel (one WriteFile) before the debug break.
extern "C" void bb_assert_marker(const char* file, int line, const char* kind) {
    char buffer[256];
    const char* name = file ? file : "?";
    const char* base = name;
    for (const char* c = name; *c; ++c) if (*c == '/' || *c == '\\') base = c + 1;
    int n = snprintf(buffer, sizeof(buffer), "M5 ASSERT %s:%d kind=%s", base, line, kind ? kind : "?");
    if (n > 0) bb_log_raw(buffer);
}
