// UWP-only, CPU-side VideoOut port contract. This is NOT a GPU renderer or a presented frame.
// It is an incremental boundary before attaching Liverpool and the CoreWindow presenter.
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
#ifdef UNCHARTED_UWP
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#endif
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "core/libraries/libs.h"
#include "core/libraries/videoout/video_out.h"
#include "core/libraries/videoout/videoout_error.h"
#include "core/memory.h"
namespace Libraries::VideoOut {
bool DriverAlive();
s32 AddFlipEventForward(Libraries::Kernel::OrbisKernelEqueue queue, s32 handle, void* udata);
}

namespace Libraries::Kernel {
int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue*,const char*);
int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue);
}

namespace {
std::mutex video_port_lock;
bool port_open;

// The port has no per-page residency bookkeeping (MemoryManager::IsValidGpuMapping only checks the
// 40-bit address limit), so "inside guest-mapped range" says nothing. What can be measured is
// whether the guest address has real, writable memory behind it: that is the difference between a
// buffer the host can fill and a pointer into the void. The device-backing question is separate.
void ReportResidency(int index, u64 address, u64 bytes) {
#ifdef _WIN32
    const auto query = [](u64 at, MEMORY_BASIC_INFORMATION& info) {
        return VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != 0;
    };
    const auto writable = [](const MEMORY_BASIC_INFORMATION& info) {
        return (info.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY)) != 0;
    };
    MEMORY_BASIC_INFORMATION before{};
    const bool queried = query(address, before);
    // Touch unconditionally: the port reserves guest memory and commits it on fault
    // (runtime_memory.c: host_commit). Conditioning the write on "already committed" tested
    // nothing, because commit-on-touch is exactly what is being verified. The write may fault;
    // the process-wide VEH is what materialises the page.
    auto* first = reinterpret_cast<volatile unsigned char*>(address);
    auto* last = reinterpret_cast<volatile unsigned char*>(address + bytes - 1);
    *first = static_cast<unsigned char>(0xA5);
    *last = static_cast<unsigned char>(0x5A);
    const bool canary_first = *first == 0xA5;
    const bool canary_last = *last == 0x5A;
    MEMORY_BASIC_INFORMATION after_first{};
    MEMORY_BASIC_INFORMATION after_last{};
    const bool first_committed = query(address, after_first) &&
                                 after_first.State == MEM_COMMIT && writable(after_first);
    const bool last_committed = query(address + bytes - 1, after_last) &&
                                after_last.State == MEM_COMMIT && writable(after_last);
    std::printf("M5 VideoOut residency[%d]: address=0x%llx before=%s/0x%lx touched_first=%d "
                "touched_last=%d first_commit=%d last_commit=%d region=%llu KiB\n",
                index, static_cast<unsigned long long>(address),
                queried && before.State == MEM_COMMIT ? "commit" : "not-commit",
                static_cast<unsigned long>(queried ? before.Protect : 0), canary_first ? 1 : 0,
                canary_last ? 1 : 0, first_committed ? 1 : 0, last_committed ? 1 : 0,
                static_cast<unsigned long long>(after_first.RegionSize >> 10));
#else
    (void)index; (void)address; (void)bytes;
#endif
}

std::vector<Libraries::Kernel::OrbisKernelEqueue> flip_queues;
std::vector<void*> flip_udata;
void PS4_SYSV_ABI SetBufferAttribute(Libraries::VideoOut::BufferAttribute* attribute,
                                      Libraries::VideoOut::PixelFormat format,u32 tiling,
                                      u32 aspect,u32 width,u32 height,u32 pitch) {
    if(!attribute) { std::puts("M5 VideoOut buffer attribute: null destination"); return; }
    // Same pure data transformation as video_out.cpp::sceVideoOutSetBufferAttribute.
    std::memset(attribute,0,sizeof(*attribute));
    attribute->pixel_format=format;
    attribute->tiling_mode=static_cast<Libraries::VideoOut::TilingMode>(tiling);
    attribute->aspect_ratio=aspect;
    attribute->width=width;
    attribute->height=height;
    attribute->pitch_in_pixel=pitch;
    attribute->option=Libraries::VideoOut::SCE_VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_NONE;
    std::printf("M5 VideoOut buffer attribute: format=0x%x tiling=%u aspect=%u size=%ux%u pitch=%u; no buffer registered\n",
                static_cast<unsigned>(format),tiling,aspect,width,height,pitch);
}
int PS4_SYSV_ABI AddFlipEvent(Libraries::Kernel::OrbisKernelEqueue queue,s32 handle,void* udata) {
    std::scoped_lock lock{video_port_lock};
    /* bbport: with the real driver alive, the flip event must live on ITS port - that is the list the
     * completion walks. Registering only in the contract left the guest waiting forever (measured: 12
     * flip IRQs, flip submitted, zero triggers for ident 6). */
    if (Libraries::VideoOut::DriverAlive()) {
        return Libraries::VideoOut::AddFlipEventForward(queue, handle, udata);
    }
    if(handle!=1 || !port_open) return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    auto* equeue=Libraries::Kernel::GetEqueue(queue);
    if(!equeue) return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
    Libraries::Kernel::EqueueEvent event{};
    event.event.ident=static_cast<u64>(Libraries::VideoOut::OrbisVideoOutInternalEventId::Flip);
    event.event.filter=Libraries::Kernel::OrbisKernelEvent::Filter::VideoOut;
    event.event.flags=Libraries::Kernel::OrbisKernelEvent::Flags::Add;
    event.event.udata=udata;
    if(!equeue->AddEvent(event)) return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
    if(std::find(flip_queues.begin(),flip_queues.end(),queue)==flip_queues.end()) {
        flip_queues.push_back(queue);
        flip_udata.push_back(udata);
    }
    std::printf("M5 VideoOut flip event registered: equeue=0x%llx; no GPU flip generated\n",
                static_cast<unsigned long long>(queue));
    return 0;
}
int PS4_SYSV_ABI DeleteFlipEvent(Libraries::Kernel::OrbisKernelEqueue queue,s32 handle) {
    std::scoped_lock lock{video_port_lock};
    if(handle!=1 || !port_open) return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    auto* equeue=Libraries::Kernel::GetEqueue(queue);
    if(!equeue) return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
    equeue->RemoveEvent(static_cast<u64>(Libraries::VideoOut::OrbisVideoOutInternalEventId::Flip),
                        Libraries::Kernel::OrbisKernelEvent::Filter::VideoOut);
    std::erase(flip_queues,queue);
    return 0;
}
int PS4_SYSV_ABI Open(Libraries::UserService::OrbisUserServiceUserId user, s32 bus,
                      s32 index, const void* params) {
    if (Libraries::VideoOut::DriverAlive()) {
        const int result = Libraries::VideoOut::sceVideoOutOpen(user, bus, index, params);
        std::printf("M5 VideoOut bridge: real open result=%d\n", result);
        std::fflush(stdout);
        return result;
    }
    (void)user;
    if(bus!=Libraries::VideoOut::SCE_VIDEO_OUT_BUS_TYPE_MAIN || index!=0)
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    // The real VideoOutDriver::Open does not use params, but keep the field visible for audit.
    std::scoped_lock lock{video_port_lock};
    if(port_open) return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    port_open=true;
    std::printf("M5 logical VideoOut port opened: handle=1 bus=%d index=%d params=%p; GPU not attached\n",
                bus,index,params);
    return 1;
}
int PS4_SYSV_ABI Close(s32 handle) {
    if (Libraries::VideoOut::DriverAlive()) {
        return Libraries::VideoOut::sceVideoOutClose(handle);
    }
    std::scoped_lock lock{video_port_lock};
    if(handle!=1 || !port_open) return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    port_open=false;
    for(auto queue:flip_queues) {
        if(auto* equeue=Libraries::Kernel::GetEqueue(queue))
            equeue->RemoveEvent(static_cast<u64>(Libraries::VideoOut::OrbisVideoOutInternalEventId::Flip),
                                Libraries::Kernel::OrbisKernelEvent::Filter::VideoOut);
    }
    flip_queues.clear();
    std::puts("M5 logical VideoOut port closed; no GPU frames submitted");
    return 0;
}
int PS4_SYSV_ABI Resolution(s32 handle,Libraries::VideoOut::SceVideoOutResolutionStatus* status) {
    if (Libraries::VideoOut::DriverAlive()) {
        return Libraries::VideoOut::sceVideoOutGetResolutionStatus(handle, status);
    }
    if(!status) return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    std::scoped_lock lock{video_port_lock};
    if(handle!=1 || !port_open) return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    *status=Libraries::VideoOut::SceVideoOutResolutionStatus{};
    return 0;
}
}

extern "C" void bbgpu_uwp_videoout_event_cleanup(Libraries::Kernel::OrbisKernelEqueue eq) {
    std::scoped_lock lock{video_port_lock};
    std::erase(flip_queues,eq);
}

// The guest's first GPU-facing call after the event queues: it hands over the frame buffers it will
// flip. This build validates and records the request; it does not allocate GPU memory yet (the
// contract has no renderer instance), so it must never pretend a buffer exists. Everything the
// guest declared is logged so the allocation can be wired to the shared VMA next.
int PS4_SYSV_ABI RegisterBuffers(s32 handle,s32 startIndex,void* const* attribs,s32 bufferNum,
                                 const Libraries::VideoOut::BufferAttribute* attribute) {
    if (Libraries::VideoOut::DriverAlive()) {
        return Libraries::VideoOut::sceVideoOutRegisterBuffers(handle, startIndex, attribs,
                                                                bufferNum, attribute);
    }
    std::scoped_lock lock{video_port_lock};
    if(handle!=1 || !port_open) return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    if(!attribs || !attribute || bufferNum<=0 || bufferNum>16) return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    const u64 bytes_per_buffer=static_cast<u64>(attribute->pitch_in_pixel)*
                               static_cast<u64>(attribute->height)*4ull;
    // The guest provides the buffers: this call registers addresses it already allocated, it does
    // not allocate. The three addresses decide the next step — if they are not inside memory the
    // port maps and backs, the first flip would read the void silently.
    auto* memory=Core::Memory::Instance();
    static const bool residency = std::getenv("BB_VIDEOOUT_RESIDENCY") != nullptr;
    u64 total_bytes=0;
    for (s32 i=0;i<bufferNum;++i) {
        const auto address=reinterpret_cast<u64>(attribs[i]);
        const bool mapped=memory && address!=0 &&
                          memory->IsValidGpuMapping(static_cast<VAddr>(address),bytes_per_buffer);
        std::printf("M5 VideoOut buffer[%d]: address=0x%llx %s\n",i,
                    static_cast<unsigned long long>(address),
                    mapped ? "inside guest-mapped range" : "NOT guest-mapped");
        if (residency && address!=0) ReportResidency(i,address,bytes_per_buffer);
        total_bytes+=bytes_per_buffer;
    }
    // Overlap between the three ranges would corrupt silently: the guest computes addresses from
    // base + i*stride, so a wrong stride aliases buffers.
    for (s32 a=0;a<bufferNum;++a) {
        for (s32 b=a+1;b<bufferNum;++b) {
            const u64 a0=reinterpret_cast<u64>(attribs[a]), b0=reinterpret_cast<u64>(attribs[b]);
            if (a0 < b0 + bytes_per_buffer && b0 < a0 + bytes_per_buffer)
                std::printf("M5 VideoOut OVERLAP between buffer %d and %d\n",a,b);
        }
    }
    if (bufferNum>1) {
        const u64 stride=reinterpret_cast<u64>(attribs[1])-reinterpret_cast<u64>(attribs[0]);
        std::printf("M5 VideoOut stride=%llu bytes for a %llu-byte buffer (slack=%lld bytes)\n",
                    static_cast<unsigned long long>(stride),
                    static_cast<unsigned long long>(bytes_per_buffer),
                    static_cast<long long>(stride)-static_cast<long long>(bytes_per_buffer));
    }
    // bbport: self-check of the real IsValidGpuMapping predicate. Four cases with known answers:
    // the first frame buffer must be valid; an address inside the 40-bit space with nothing mapped
    // must be rejected; a high address plus a large size used to wrap and pass; an unaligned one
    // must be rejected.
    if (memory != nullptr && bufferNum > 0) {
        const u64 good = reinterpret_cast<u64>(attribs[0]);
        const bool p1 = memory->IsValidGpuMapping(static_cast<VAddr>(good), bytes_per_buffer);
        const bool p2 = memory->IsValidGpuMapping(static_cast<VAddr>(0x0000'0abc'0000ull), 4096);
        const bool p4 = memory->IsValidGpuMapping(static_cast<VAddr>(0xffff'ffff'ffff'f000ull),
                                                  0x2000);
        const bool p5 = memory->IsValidGpuMapping(static_cast<VAddr>(good + 1), bytes_per_buffer);
        std::printf("M5 GPU mapping predicate: buffer0=%d unmapped_region=%d overflow=%d "
                    "unaligned=%d\n", p1 ? 1 : 0, p2 ? 1 : 0, p4 ? 1 : 0, p5 ? 1 : 0);
        std::fflush(stdout);
    }
    std::printf("M5 VideoOut register buffers: handle=%d start=%d count=%d format=0x%x tiling=%u "
                "%ux%u pitch=%u => %llu bytes/buffer, %llu bytes total; logical only (no GPU "
                "allocation in this build)\n",
                handle,startIndex,bufferNum,static_cast<unsigned>(attribute->pixel_format),
                static_cast<unsigned>(attribute->tiling_mode),attribute->width,attribute->height,
                attribute->pitch_in_pixel,static_cast<unsigned long long>(bytes_per_buffer),
                static_cast<unsigned long long>(bytes_per_buffer)*static_cast<u64>(bufferNum));
    std::fflush(stdout);
    return 0;
}

// Flip rate is a policy knob with no frame source yet: accept a sane value and record it.
int PS4_SYSV_ABI SetFlipRate(s32 handle,u32 rate) {
    if (Libraries::VideoOut::DriverAlive()) {
        return Libraries::VideoOut::sceVideoOutSetFlipRate(handle, static_cast<s32>(rate));
    }
    std::scoped_lock lock{video_port_lock};
    if(handle!=1 || !port_open) return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    std::printf("M5 VideoOut flip rate: handle=%d rate=%u; recorded, no frame source\n",handle,rate);
    std::fflush(stdout);
    return 0;
}

extern "C" int32_t runtime_audio_init_bridge();
extern "C" int32_t runtime_audio_open_bridge(int32_t user, int32_t type, int32_t index,
                                               uint32_t length, uint32_t freq, uint32_t param);

// Audio is out of scope for M5 (video), but the guest's platform init asks for it. Acknowledging the
// init lets the guest continue; no output port is created, so nothing ever blocks waiting for a
// consumer. Returning an error here instead makes some titles skip audio entirely, which is the
// fallback if a later gate starts waiting on a port that does not exist.
int PS4_SYSV_ABI AudioOutInit() {
    const int result = runtime_audio_init_bridge();
    std::printf("M5 AudioOut init: runtime timer backend result=%d\n", result);
    std::fflush(stdout);
    return result;
}

// sceAudioOutOpen: the honest answer is "no device". Returning a handle with no consumer would make
// the guest queue audio and wait for a port that never exists; a negative result lets it disable
// audio and continue. Handles are signed and callers test `handle < 0`, so -1 is unambiguous.
int PS4_SYSV_ABI AudioOutOpen(s32 user, s32 type, s32 index, u32 len, u32 freq, u32 param) {
    const int result = runtime_audio_open_bridge(user, type, index, len, freq, param);
    std::printf("M5 AudioOut open: user=%d type=%d index=%d len=%u freq=%u param=0x%x -> %d\n",
                user, type, index, len, freq, param, result);
    std::fflush(stdout);
    return result;
}

// Pad: the guest needs a real device or it never leaves the title screen. The buttons come from the
// scripted file the loader points BB_PAD_FILE at (LocalState\pad.txt), re-read when it changes, so an
// automated run can press Cross without a controller attached to the console.
namespace {
constexpr u32 PAD_CROSS = 0x4000;
constexpr u32 PAD_CIRCLE = 0x2000;
constexpr u32 PAD_SQUARE = 0x8000;
constexpr u32 PAD_TRIANGLE = 0x1000;
constexpr u32 PAD_L1 = 0x400, PAD_R1 = 0x800, PAD_L2 = 0x100, PAD_R2 = 0x200;
constexpr u32 PAD_L3 = 0x1, PAD_R3 = 0x2, PAD_OPTIONS = 0x8, PAD_TOUCHPAD = 0x100000;
constexpr u32 PAD_UP = 0x10, PAD_DOWN = 0x40, PAD_LEFT = 0x80, PAD_RIGHT = 0x20;

struct PadScript {
    u32 buttons{};
    std::array<u8, 4> sticks{128, 128, 128, 128};
};

/// Reread BB_PAD_FILE at most every 50 ms; the tokens are the same names runtime_pad.c accepts.
PadScript& CurrentPadScript() {
    static PadScript script{};
    static u64 last_check{};
    static bool logged{};
    const u64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (now - last_check < 50) {
        return script;
    }
    last_check = now;
    const char* path = std::getenv("BB_PAD_FILE");
    if (path == nullptr || path[0] == '\0') {
        return script;
    }
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return script;
    }
    static const std::pair<const char*, u32> names[] = {
        {"cross", PAD_CROSS},       {"circle", PAD_CIRCLE},   {"square", PAD_SQUARE},
        {"triangle", PAD_TRIANGLE}, {"l1", PAD_L1},           {"r1", PAD_R1},
        {"l2", PAD_L2},             {"r2", PAD_R2},           {"l3", PAD_L3},
        {"r3", PAD_R3},             {"options", PAD_OPTIONS}, {"touchpad", PAD_TOUCHPAD},
        {"up", PAD_UP},             {"down", PAD_DOWN},       {"left", PAD_LEFT},
        {"right", PAD_RIGHT},
    };
    PadScript next{};
    char token[64];
    while (std::fscanf(file, "%63s", token) == 1) {
        for (const auto& [name, bit] : names) {
            if (std::strcmp(token, name) == 0) {
                next.buttons |= bit;
            }
        }
        static const char* axes[] = {"lx=", "ly=", "rx=", "ry="};
        for (u32 i = 0; i < 4; ++i) {
            if (std::strncmp(token, axes[i], 3) == 0) {
                const int value = std::atoi(token + 3);
                next.sticks[i] = static_cast<u8>(value < 0 ? 0 : value > 255 ? 255 : value);
            }
        }
    }
    std::fclose(file);
    if (!logged || next.buttons != script.buttons) {
        logged = true;
        std::printf("M5 Pad: scripted buttons=0x%x\n", next.buttons);
        std::fflush(stdout);
    }
    script = next;
    return script;
}

#ifdef UNCHARTED_UWP
/// Live Xbox controller: the title asks for Cross and the console has no keyboard, so the guest's
/// pad mirrors the real gamepad. The scripted file still overrides it when it holds buttons.
struct PadLive {
    u32 buttons{};
    std::array<u8, 4> sticks{128, 128, 128, 128};
};

PadLive ReadLivePad() {
    using namespace winrt::Windows::Gaming::Input;
    PadLive live{};
    static Gamepad gamepad{nullptr};
    static bool logged{};
    if (!gamepad) {
        winrt::Windows::Foundation::Collections::IVectorView<Gamepad> pads = Gamepad::Gamepads();
        if (pads.Size() > 0) {
            gamepad = pads.GetAt(0);
            if (!logged) {
                logged = true;
                std::printf("M5 Pad: live gamepad attached\n");
                std::fflush(stdout);
            }
        }
    }
    if (!gamepad) {
        return live;
    }
    const GamepadReading reading = gamepad.GetCurrentReading();
    const auto buttons = reading.Buttons;
    const auto has = [buttons](GamepadButtons bit) {
        return (static_cast<u32>(buttons) & static_cast<u32>(bit)) != 0;
    };
    if (has(GamepadButtons::A)) live.buttons |= PAD_CROSS;
    if (has(GamepadButtons::B)) live.buttons |= PAD_CIRCLE;
    if (has(GamepadButtons::X)) live.buttons |= PAD_SQUARE;
    if (has(GamepadButtons::Y)) live.buttons |= PAD_TRIANGLE;
    if (has(GamepadButtons::LeftShoulder)) live.buttons |= PAD_L1;
    if (has(GamepadButtons::RightShoulder)) live.buttons |= PAD_R1;
    if (has(GamepadButtons::LeftThumbstick)) live.buttons |= PAD_L3;
    if (has(GamepadButtons::RightThumbstick)) live.buttons |= PAD_R3;
    if (has(GamepadButtons::Menu)) live.buttons |= PAD_OPTIONS;
    if (has(GamepadButtons::View)) live.buttons |= PAD_TOUCHPAD;
    if (has(GamepadButtons::DPadUp)) live.buttons |= PAD_UP;
    if (has(GamepadButtons::DPadDown)) live.buttons |= PAD_DOWN;
    if (has(GamepadButtons::DPadLeft)) live.buttons |= PAD_LEFT;
    if (has(GamepadButtons::DPadRight)) live.buttons |= PAD_RIGHT;
    if (reading.LeftTrigger > 0.1) live.buttons |= PAD_L2;
    if (reading.RightTrigger > 0.1) live.buttons |= PAD_R2;
    const auto axis = [](double value) {
        return static_cast<u8>(std::clamp<int>(static_cast<int>((value + 1.0) * 127.5), 0, 255));
    };
    live.sticks[0] = axis(reading.LeftThumbstickX);
    live.sticks[1] = static_cast<u8>(255 - axis(reading.LeftThumbstickY)); // PS4: 0 is up
    live.sticks[2] = axis(reading.RightThumbstickX);
    live.sticks[3] = static_cast<u8>(255 - axis(reading.RightThumbstickY));
    return live;
}
#endif

/// scePadData: 120 bytes, layout from src/runtime_pad.c (static_assert there).
struct PadDataLayout {
    u32 buttons;
    u8 left_x, left_y, right_x, right_y;
    u8 l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    u8 touch_count, touch_reserve[3];
    u32 touch_held_time;
    u8 touches[2][8];
    u8 connected, pad0[3];
    u64 timestamp;
    u8 extension[16];
    u8 connected_count, reserve[2], unique_length, unique[12];
};
static_assert(sizeof(PadDataLayout) == 120, "ScePadData must match runtime_pad.c");

/// scePadControllerInformation: 28 bytes, layout from src/runtime_pad.c.
struct PadInfoLayout {
    float pixel_density;
    u16 resolution_x, resolution_y;
    u8 dead_zone_left, dead_zone_right, connection_type, connected_count;
    u8 connected, pad[3];
    s32 device_class;
    u8 reserve[8];
};
static_assert(sizeof(PadInfoLayout) == 28, "ScePadControllerInformation must match runtime_pad.c");
} // namespace

int PS4_SYSV_ABI PadInit() {
    static std::atomic<u32> calls{0};
    const u32 seen = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen <= 2) {
        std::printf("M5 Pad init: one scripted device (#%u)\n", seen);
        std::fflush(stdout);
    }
    return 0;
}
int PS4_SYSV_ABI PadOpen(s32 user, s32 type, s32 index, void* param) {
    (void)type;
    (void)index;
    (void)param;
    /* Log every distinct (user, type) once: the title gate depends on which user the game opens. */
    static std::mutex log_mutex;
    static std::vector<std::pair<s32, s32>> logged;
    {
        std::scoped_lock lock{log_mutex};
        const auto key = std::pair<s32, s32>{user, type};
        if (std::ranges::find(logged, key) == logged.end()) {
            logged.push_back(key);
            std::printf("M5 Pad open: user=%d type=%d index=%d -> handle 1\n", user, type, index);
            std::fflush(stdout);
        }
    }
    return 1; // PAD_HANDLE, matching the validated PC path in runtime_pad.c
}
int PS4_SYSV_ABI PadGetControllerInformation(s32 handle, void* info) {
    (void)handle;
    if (info == nullptr) {
        return -1;
    }
    auto* out = static_cast<PadInfoLayout*>(info);
    *out = {};
    out->pixel_density = 44.86f;
    out->resolution_x = 1920;
    out->resolution_y = 943;
    out->dead_zone_left = 2;
    out->dead_zone_right = 2;
    out->connection_type = 0;
    out->connected_count = 1;
    out->connected = 1;
    out->device_class = 0;
    return 0;
}
int PS4_SYSV_ABI PadRead(s32 handle, void* data, s32 count) {
    (void)handle;
    if (data == nullptr) {
        return -1;
    }
    auto* out = static_cast<PadDataLayout*>(data);
    *out = {};
    /* Identity quaternion: the working PC path sets this (runtime_pad.c) and the title screen
     * rejects a zero orientation even when the buttons are right. */
    out->orientation[3] = 1.0f;
    const PadScript& script = CurrentPadScript();
    u32 buttons = script.buttons;
    std::array<u8, 4> sticks = script.sticks;
#ifdef UNCHARTED_UWP
    const PadLive live = ReadLivePad();
    buttons |= live.buttons;
    if (live.buttons != 0) {
        sticks = live.sticks;
    }
#endif
    out->buttons = buttons;
#ifdef UNCHARTED_UWP
    {
        static std::atomic<u32> reads{0};
        const u32 count = reads.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count % 500 == 0) {
            std::printf("M5 Pad read #%u buttons=0x%x live=0x%x sticks=%u,%u,%u,%u\n", count,
                        buttons, live.buttons, sticks[0], sticks[1], sticks[2], sticks[3]);
            std::fflush(stdout);
        }
    }
#endif
    out->left_x = sticks[0];
    out->left_y = sticks[1];
    out->right_x = sticks[2];
    out->right_y = sticks[3];
    out->connected = 1;
    out->connected_count = 1;
    out->timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    // Buffered read returns the number of samples, as the working PC path does (runtime_pad.c).
    if (count < 1) {
        return -1;
    }
    return count < 1 ? -1 : 1;
}
int PS4_SYSV_ABI PadSetLightBar(s32 handle, const void* param) {
    (void)handle;
    (void)param;
    return 0;
}
int PS4_SYSV_ABI PadResetLightBar(s32 handle) {
    (void)handle;
    return 0;
}
int PS4_SYSV_ABI PadSetVibration(s32 handle, const void* param) {
    (void)handle;
    (void)param;
    return 0;
}
/// The guest polls this one more than scePadRead; same payload, returns 0 on success.
int PS4_SYSV_ABI PadReadState(s32 handle, void* data) {
    PadRead(handle, data, 1);
    return 0;
}
int PS4_SYSV_ABI PadClose(s32 handle) {
    (void)handle;
    return 0;
}
int PS4_SYSV_ABI PadResetOrientation(s32 handle) {
    (void)handle;
    return 0;
}
int PS4_SYSV_ABI PadSetTiltCorrectionState(s32 handle, s32 state) {
    (void)handle;
    (void)state;
    return 0;
}
int PS4_SYSV_ABI PadSetAngularVelocityDeadbandState(s32 handle, s32 state) {
    (void)handle;
    (void)state;
    return 0;
}

// Colour settings are display preferences, not state the renderer reads: acknowledge them so the
// guest's video initialisation can finish, and keep the values out of the pipeline until a frame
// exists. Logging the first call is what makes the gate visible as evidence.
int PS4_SYSV_ABI ColorSettingsSetGamma(void* settings) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=2) { std::printf("M5 VideoOut gamma: settings=%p acknowledged (#%u)\n",settings,seen); std::fflush(stdout); }
    return 0;
}
int PS4_SYSV_ABI AdjustColor(void* settings) {
    static std::atomic<u32> calls{0};
    const u32 seen=calls.fetch_add(1,std::memory_order_relaxed)+1;
    if(seen<=2) { std::printf("M5 VideoOut adjust colour: settings=%p acknowledged (#%u)\n",settings,seen); std::fflush(stdout); }
    return 0;
}

// bbport: how many equeues are registered for flip events. The present pipeline must never invent an
// ident: with no registration there is nothing to signal, and the correct behaviour is to count the
// completed flip without an event.
extern "C" unsigned bbgpu_uwp_videoout_flip_queue_count(void) {
    std::scoped_lock lock{video_port_lock};
    return static_cast<unsigned>(flip_queues.size());
}

// bbport: signal every registered flip queue on a vblank tick. The ident comes from the registration
// (AddFlipEvent stored it with the event), never synthesised here.
extern "C" unsigned bbgpu_uwp_videoout_signal_flip(unsigned long long tick) {
    std::scoped_lock lock{video_port_lock};
    unsigned signalled = 0;
    for (size_t i = 0; i < flip_queues.size(); ++i) {
        auto* eq = Libraries::Kernel::GetEqueue(flip_queues[i]);
        if (!eq) continue;
        eq->TriggerEvent(static_cast<u64>(Libraries::VideoOut::OrbisVideoOutInternalEventId::Flip),
                         Libraries::Kernel::OrbisKernelEvent::Filter::VideoOut,
                         i < flip_udata.size() ? flip_udata[i] : nullptr);
        ++signalled;
    }
    if (signalled && tick <= 4) {
        std::printf("M5 VideoOut flip event: tick=%llu signalled=%u registered=%zu\n",
                    tick, signalled, flip_queues.size());
        std::fflush(stdout);
    }
    return signalled;
}

extern "C" void bbgpu_uwp_videoout_contract_register(Core::Loader::SymbolsResolver* sym) {
    sym->AddSymbol("DYhhWbJSeRg","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(ColorSettingsSetGamma));
    sym->AddSymbol("pv9CI5VC+R0","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(AdjustColor));

    // Register only implemented logical operations; leave frame/vblank APIs unresolved instead
    // of returning fake success or pretending to present a guest buffer.
    sym->AddSymbol("Up36PTk687E","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(Open));
    sym->AddSymbol("uquVH4-Du78","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(Close));
    sym->AddSymbol("6kPnj51T62Y","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(Resolution));
    sym->AddSymbol("HXzjK9yI30k","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(AddFlipEvent));
    sym->AddSymbol("-Ozn0F1AFRg","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(DeleteFlipEvent));
    sym->AddSymbol("i6-sR91Wt-4","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(SetBufferAttribute));
    sym->AddSymbol("w3BY+tAEiQY","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(RegisterBuffers));
    sym->AddSymbol("CBiu4mCE1DA","libSceVideoOut","libSceVideoOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(SetFlipRate));
    // Audio lives here only because this is the logical platform-contract file; the symbol is
    // registered under its own library so the guest resolves it as libSceAudioOut.
    sym->AddSymbol("JfEPXVxhFqA","libSceAudioOut","libSceAudioOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(AudioOutInit));
    sym->AddSymbol("ekNvsT22rsY","libSceAudioOut","libSceAudioOut",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(AudioOutOpen));
    // Pad: no device in this build (see the implementations above).
    sym->AddSymbol("hv1luiJrqQM","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadInit));
    sym->AddSymbol("xk0AcarP3V4","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadOpen));
    sym->AddSymbol("gjP9-KQzoUk","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadGetControllerInformation));
    sym->AddSymbol("q1cHNfGycLI","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadRead));
    sym->AddSymbol("RR4novUEENY","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadSetLightBar));
    sym->AddSymbol("DscD1i9HX1w","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadResetLightBar));
    sym->AddSymbol("yFVnOdGxvZY","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadSetVibration));
    sym->AddSymbol("YndgXqQVV7c","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadReadState));
    sym->AddSymbol("6ncge5+l5Qs","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadClose));
    sym->AddSymbol("rIZnR6eSpvk","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadResetOrientation));
    sym->AddSymbol("vDLMoJLde8I","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadSetTiltCorrectionState));
    sym->AddSymbol("r44mAxdSG+U","libScePad","libScePad",
                   Core::Loader::SymbolType::Function,reinterpret_cast<u64>(PadSetAngularVelocityDeadbandState));
}

extern "C" int bbgpu_uwp_videoout_contract_selftest(void) {
    using namespace Libraries::VideoOut;
    if(Open(1,SCE_VIDEO_OUT_BUS_TYPE_MAIN,1,nullptr)!=ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE) return 1;
    if(Open(1,SCE_VIDEO_OUT_BUS_TYPE_AUX_SOCIAL_SCREEN,0,nullptr)!=ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE) return 2;
    if(Open(1,SCE_VIDEO_OUT_BUS_TYPE_MAIN,0,nullptr)!=1) return 3;
    if(Open(1,SCE_VIDEO_OUT_BUS_TYPE_MAIN,0,nullptr)!=ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY) return 4;
    SceVideoOutResolutionStatus status{};
    if(Resolution(1,&status) || status.full_width!=1280 || status.full_height!=720) return 5;
    BufferAttribute attrib{};
    SetBufferAttribute(&attrib,PixelFormat::A8B8G8R8Srgb,0,0,1280,720,1280);
    if(attrib.pixel_format!=PixelFormat::A8B8G8R8Srgb ||
       attrib.tiling_mode!=TilingMode::Tile || attrib.width!=1280 ||
       attrib.height!=720 || attrib.pitch_in_pixel!=1280 ||
       attrib.option!=SCE_VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_NONE || attrib.reserved1) return 15;
    Libraries::Kernel::OrbisKernelEqueue eq=0;
    if(Libraries::Kernel::sceKernelCreateEqueue(&eq,"M5-vo-flip-contract")) return 9;
    constexpr auto flip=static_cast<u64>(OrbisVideoOutInternalEventId::Flip);
    constexpr auto filter=Libraries::Kernel::OrbisKernelEvent::Filter::VideoOut;
    if(AddFlipEvent(eq,1,reinterpret_cast<void*>(uintptr_t(0x1234))) ||
       !Libraries::Kernel::GetEqueue(eq)->EventExists(flip,filter)) return 10;
    Libraries::Kernel::OrbisKernelEvent event{};
    Libraries::Kernel::OrbisKernelUseconds poll=0;
    if(Libraries::Kernel::GetEqueue(eq)->WaitForEvents(&event,1,&poll)!=0) return 11;
    if(DeleteFlipEvent(eq,1) || Libraries::Kernel::GetEqueue(eq)->EventExists(flip,filter)) return 12;
    if(AddFlipEvent(eq,1,nullptr) || Libraries::Kernel::sceKernelDeleteEqueue(eq) ||
       !flip_queues.empty()) return 13;
    if(Close(1)) return 14;
    if(Close(2)!=ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE) return 6;
    if(Resolution(1,&status)!=ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE || Close(1)!=ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE) return 7;
    if(Open(1,SCE_VIDEO_OUT_BUS_TYPE_MAIN,0,nullptr)!=1 || Close(1)) return 8;
    std::puts("PASS: logical VideoOut Open/Close/reopen, flip equeue add/delete/cleanup, buffer attribute (no synthetic frame), invalid bus/index and resolution; rendering withheld");
    return 0;
}
#endif
