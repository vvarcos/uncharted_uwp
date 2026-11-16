// AppContainer-safe extraction of the real shadPS4 event-queue runtime.
// Kept separate from bbgpu.cpp so CPU-only M4 does not pull SDL/window/Vulkan initialization.
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "common/types.h"
#include "core/libraries/videodec/videodec2.h"
#include "core/libraries/videoout/video_out.h"

namespace Libraries::VideoOut {
void EnsureDriver();
}

#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/libs.h"

extern "C" int32_t* runtime_errno(void);
extern "C" void bb_log_raw(const char* text);
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
extern "C" void bbgpu_uwp_videoout_contract_register(Core::Loader::SymbolsResolver* resolver);
extern "C" void bbgpu_uwp_gnm_event_register(Core::Loader::SymbolsResolver* resolver);
// The real driver. RegisterLib installs its LIB_FUNCTION table and creates the Liverpool that owns
// the GPU command processor thread; until now nothing called it on this target, so every GNM import
// had to be answered by a contract stub.
namespace Libraries::GnmDriver {
void RegisterLib(Core::Loader::SymbolsResolver* sym);
}
#endif

namespace {
struct UwpKernelSymbol { std::string nid; u64 address; };
std::vector<UwpKernelSymbol> symbols;
std::mutex symbols_mutex;
std::once_flag register_once;

// Step 1 of the plan: markers on the raw channel, one WriteFile each, inside the registration
// itself. A marker that never appears is evidence that the code is not on the executed path.
unsigned SymbolCountLocked() { return static_cast<unsigned>(symbols.size()); }
unsigned SymbolCount() { std::lock_guard lock{symbols_mutex}; return SymbolCountLocked(); }
void Marker(const char* text) {
    char buffer[192];
    std::snprintf(buffer, sizeof(buffer), "%s (symbols=%u)", text, SymbolCount());
    bb_log_raw(buffer);
}
bool RealGnmDriver() {
    const char* env = std::getenv("BB_REAL_GNM");
    return env && env[0] == '1';
}
}

namespace Core::Loader {
void SymbolsResolver::AddSymbol(const char* nid,const char*,const char*,SymbolType,u64 address) {
    std::lock_guard lock{symbols_mutex};
    symbols.push_back({nid,address});
}
}

namespace Libraries::Kernel {
boost::asio::io_context io_context;
void KernelSignalRequest() {
    struct Service {
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard{boost::asio::make_work_guard(io_context)};
        std::thread thread{[] { io_context.run(); }};
        ~Service() { io_context.stop(); if(thread.joinable()) thread.join(); }
    };
    static Service service;
    (void)service;
}
int* PS4_SYSV_ABI __Error() { return runtime_errno(); }
void RegisterEventQueue(Core::Loader::SymbolsResolver* resolver);
int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue*,const char*);
int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue);
int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue,OrbisKernelEvent*,int,int*,OrbisKernelUseconds*);
int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue,int);
int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue,int,void*);
int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue,int);
int PS4_SYSV_ABI sceKernelAddTimerEvent(OrbisKernelEqueue,int,OrbisKernelUseconds,void*);
int PS4_SYSV_ABI sceKernelDeleteTimerEvent(OrbisKernelEqueue,int);
}

extern "C" void bbgpu_uwp_kernel_register(void) {
    std::call_once(register_once,[] {
        Core::Loader::SymbolsResolver resolver;
        Marker("M5 HLE: register enter");
        Libraries::Kernel::RegisterEventQueue(&resolver);
        Marker("M5 HLE: equeue registered");
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
        /* bbport: ONE table. The loader only ever asks bbgpu_uwp_kernel_resolve, so the real driver
         * has to register into THIS resolver. It goes first: the resolve scan returns the first
         * match, so a contract stub registered later can never shadow the driver's implementation. */
        if (RealGnmDriver()) {
            Marker("M5 HLE: real GNM driver = 1, GnmDriver::RegisterLib enter");
            Libraries::GnmDriver::RegisterLib(&resolver);
            Marker("M5 HLE: GnmDriver::RegisterLib returned");
        } else {
            Marker("M5 HLE: real GNM driver = 0, contract stubs only");
        }
        bbgpu_uwp_videoout_contract_register(&resolver);
        /* bbport: now the default. Without the driver the guest flip event is never signalled, because the
         * completion walks the port's flip list. BB_VIDEOOUT_DRIVER=0 restores the contract-only path. */
        {
            const char* d = std::getenv("BB_VIDEOOUT_DRIVER");
            if (d == nullptr || d[0] != '0') {
                Libraries::VideoOut::EnsureDriver();
            }
        }
        Libraries::Videodec2::RegisterLib(&resolver);
        Marker("M5 HLE: videodec2 registered (query + queue)");
        bbgpu_uwp_gnm_event_register(&resolver);
        Marker("M5 HLE: gnm event contract registered");
#endif
    });
}

extern "C" uintptr_t bbgpu_uwp_kernel_resolve(const char* scoped_nid) {
    if(!scoped_nid) return 0;
    const char* hash=std::strchr(scoped_nid,'#');
    const size_t length=hash ? static_cast<size_t>(hash-scoped_nid) : std::strlen(scoped_nid);
    {
        std::lock_guard lock{symbols_mutex};
        for(const auto& symbol:symbols)
            if(symbol.nid.size()==length&&!std::memcmp(symbol.nid.data(),scoped_nid,length))
                return static_cast<uintptr_t>(symbol.address);
    }
    return 0;
}

extern "C" unsigned bbgpu_uwp_kernel_symbol_count(void) {
    std::lock_guard lock{symbols_mutex};
    return static_cast<unsigned>(symbols.size());
}

/* bbport: on UWP SetCurrentThreadName is a no-op (SetThreadDescription is outside the AppContainer
 * contract), so a log line can only say "thread 0x%08x". This registry keeps the name each thread
 * gave itself, keyed by the same hash the logs print, so a stuck wait can name its holder instead of
 * a number. Diagnostic only: nothing reads it on the hot path. */
namespace {
std::mutex thread_tags_mutex;
std::vector<std::pair<unsigned, std::string>> thread_tags;

unsigned CurrentThreadHash() {
    return static_cast<unsigned>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}
} // namespace

extern "C" void bbgpu_uwp_thread_tag(const char* name) {
    if (name == nullptr) return;
    const unsigned hash = CurrentThreadHash();
    std::lock_guard lock{thread_tags_mutex};
    for (auto& tag : thread_tags) {
        if (tag.first == hash) {
            tag.second = name;
            return;
        }
    }
    thread_tags.emplace_back(hash, name);
}

extern "C" const char* bbgpu_uwp_thread_name(unsigned hash) {
    std::lock_guard lock{thread_tags_mutex};
    for (const auto& tag : thread_tags) {
        if (tag.first == hash) return tag.second.c_str();
    }
    return "?";
}

extern "C" int bbgpu_uwp_kernel_selftest(void) {
    using namespace Libraries::Kernel;
    bbgpu_uwp_kernel_register();
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
    constexpr unsigned expected=25;
    if(!bbgpu_uwp_kernel_resolve("Up36PTk687E#C#D") ||
       !bbgpu_uwp_kernel_resolve("uquVH4-Du78#C#D") ||
       !bbgpu_uwp_kernel_resolve("HXzjK9yI30k#C#D") ||
       !bbgpu_uwp_kernel_resolve("i6-sR91Wt-4#C#D") ||
       !bbgpu_uwp_kernel_resolve("b0xyllnVY-I#B#C")) return 12;
#else
    constexpr unsigned expected=17;
#endif
    if(bbgpu_uwp_kernel_symbol_count()!=expected ||
       !bbgpu_uwp_kernel_resolve("D0OdFMjp46I#p#J") ||
       !bbgpu_uwp_kernel_resolve("fzyMKs9kim0#p#J")) return 1;
    OrbisKernelEqueue queue=0;
    if(sceKernelCreateEqueue(&queue,"M4-event-selftest") || queue<0x40000) return 2;
    OrbisKernelEvent event{};
    int count=-1;
    OrbisKernelUseconds timeout=0;
    if(!sceKernelWaitEqueue(queue,&event,1,&count,&timeout)) return 3;
    if(sceKernelAddUserEvent(queue,10)) return 4;
    if(sceKernelTriggerUserEvent(queue,10,reinterpret_cast<void*>(uintptr_t(0x1234)))) return 5;
    timeout=100000;
    if(sceKernelWaitEqueue(queue,&event,1,&count,&timeout) || count!=1 ||
       event.ident!=10 || event.filter!=OrbisKernelEvent::Filter::User ||
       event.udata!=reinterpret_cast<void*>(uintptr_t(0x1234))) return 6;
    if(sceKernelDeleteUserEvent(queue,10)) return 7;
    if(sceKernelAddTimerEvent(queue,11,2000,nullptr)) return 8;
    timeout=1000000;
    if(sceKernelWaitEqueue(queue,&event,1,&count,&timeout) || count!=1 ||
       event.ident!=11 || event.filter!=OrbisKernelEvent::Filter::Timer) return 9;
    if(sceKernelDeleteTimerEvent(queue,11)) return 10;
    if(sceKernelDeleteEqueue(queue)) return 11;
    std::puts("PASS: real shadPS4 equeue create, timed wait, user trigger, timer and delete");
    return 0;
}
