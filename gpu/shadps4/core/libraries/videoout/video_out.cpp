// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/libs.h"
#include "core/libraries/system/userservice.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/video_out.h"
#include "core/libraries/videoout/videoout_error.h"
#include "core/platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

#ifdef UNCHARTED_UWP
// Raw marker channel: the flip IRQ runs on the interrupt thread.
extern "C" void bb_log_raw(const char* text);
#endif

extern std::unique_ptr<Vulkan::Presenter> presenter;

#ifdef UNCHARTED_UWP
/* bbport: installed by RegisterLib; sits at file scope because a declaration inside a function body is
 * not valid C++. */
extern "C" void BbSetFlipPendingProbe(int (*probe)(void));
#endif
namespace Libraries::VideoOut {

static std::unique_ptr<VideoOutDriver> driver;

void PS4_SYSV_ABI sceVideoOutSetBufferAttribute(BufferAttribute* attribute, PixelFormat pixelFormat,
                                                u32 tilingMode, u32 aspectRatio, u32 width,
                                                u32 height, u32 pitchInPixel) {
    LOG_INFO(Lib_VideoOut,
             "pixelFormat = {}, tilingMode = {}, aspectRatio = {}, width = {}, height = {}, "
             "pitchInPixel = {}",
             GetPixelFormatString(pixelFormat), tilingMode, aspectRatio, width, height,
             pitchInPixel);

    std::memset(attribute, 0, sizeof(BufferAttribute));
    attribute->pixel_format = static_cast<PixelFormat>(pixelFormat);
    attribute->tiling_mode = static_cast<TilingMode>(tilingMode);
    attribute->aspect_ratio = aspectRatio;
    attribute->width = width;
    attribute->height = height;
    attribute->pitch_in_pixel = pitchInPixel;
    attribute->option = SCE_VIDEO_OUT_BUFFER_ATTRIBUTE_OPTION_NONE;
}

s32 PS4_SYSV_ABI sceVideoOutAddFlipEvent(Kernel::OrbisKernelEqueue eq, s32 handle, void* udata) {
    LOG_INFO(Lib_VideoOut, "handle = {}", handle);

    auto* port = driver->GetPort(handle);
    if (port == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    auto equeue = Kernel::GetEqueue(eq);
    if (equeue == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
    }

    Kernel::EqueueEvent event{};
    event.event.ident = static_cast<u64>(OrbisVideoOutInternalEventId::Flip);
    event.event.filter = Kernel::OrbisKernelEvent::Filter::VideoOut;
    event.event.flags = Kernel::OrbisKernelEvent::Flags::Add;
    event.event.udata = udata;
    event.event.fflags = 0;
    event.event.data = 0;
    event.data = port;
    equeue->AddEvent(event);

#ifdef UNCHARTED_UWP
    /* bbport: this registration owns its payload - the flip returns the udata given here - so a
     * time-driven clock may complete it. A user event has no such payload and must not be signalled by
     * the port. */
    std::printf("M5 equeue reg: eq=%p name=%s ident=%d type=flip udata=%p handle=%d\n", eq,
                std::string(equeue->GetName()).c_str(),
                static_cast<int>(OrbisVideoOutInternalEventId::Flip), udata, handle);
    std::fflush(stdout);
#endif

    port->flip_events.push_back(eq);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutDeleteFlipEvent(Kernel::OrbisKernelEqueue eq, s32 handle) {
    auto* port = driver->GetPort(handle);
    if (port == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    auto equeue = Kernel::GetEqueue(eq);
    if (equeue == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
    }
    equeue->RemoveEvent(handle, Kernel::OrbisKernelEvent::Filter::VideoOut);
    port->flip_events.erase(find(port->flip_events.begin(), port->flip_events.end(), eq));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutAddVblankEvent(Kernel::OrbisKernelEqueue eq, s32 handle, void* udata) {
    LOG_INFO(Lib_VideoOut, "handle = {}", handle);

    auto* port = driver->GetPort(handle);
    if (port == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    auto equeue = Kernel::GetEqueue(eq);
    if (equeue == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
    }

    Kernel::EqueueEvent event{};
    event.event.ident = static_cast<u64>(OrbisVideoOutInternalEventId::Vblank);
    event.event.filter = Kernel::OrbisKernelEvent::Filter::VideoOut;
    event.event.flags = Kernel::OrbisKernelEvent::Flags::Add;
    event.event.udata = udata;
    event.event.fflags = 0;
    event.event.data = 0;
    event.data = port;
    equeue->AddEvent(event);

    port->vblank_events.push_back(eq);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutDeleteVblankEvent(Kernel::OrbisKernelEqueue eq, s32 handle) {
    auto* port = driver->GetPort(handle);
    if (port == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    auto equeue = Kernel::GetEqueue(eq);
    if (equeue == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT_QUEUE;
    }
    equeue->RemoveEvent(handle, Kernel::OrbisKernelEvent::Filter::VideoOut);
    port->vblank_events.erase(find(port->vblank_events.begin(), port->vblank_events.end(), eq));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutRegisterBuffers(s32 handle, s32 startIndex, void* const* addresses,
                                            s32 bufferNum, const BufferAttribute* attribute) {
    if (!addresses || !attribute) {
        LOG_ERROR(Lib_VideoOut, "Addresses are null");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    }

    auto* port = driver->GetPort(handle);
    if (!port || !port->is_open) {
        LOG_ERROR(Lib_VideoOut, "Invalid handle = {}", handle);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    return driver->RegisterBuffers(port, startIndex, addresses, bufferNum, attribute);
}

s32 PS4_SYSV_ABI sceVideoOutSetFlipRate(s32 handle, s32 rate) {
    LOG_TRACE(Lib_VideoOut, "called");
    driver->GetPort(handle)->flip_rate = rate;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutIsFlipPending(s32 handle) {
    LOG_TRACE(Lib_VideoOut, "called");
    auto* port = driver->GetPort(handle);
    std::unique_lock lock{port->port_mutex};
    s32 pending = port->flip_status.flip_pending_num;
    return pending;
}

s32 PS4_SYSV_ABI sceVideoOutSubmitFlip(s32 handle, s32 bufferIndex, s32 flipMode, s64 flipArg) {
    auto* port = driver->GetPort(handle);
#ifdef UNCHARTED_UWP
    /* bbport: the guest blocks on a flip that never arrives, and flip_pending_num is zero while it waits.
     * That can only mean no request was ever enqueued, so the question is whether the guest called this
     * at all or we refused it. The reason says which, and names the fix. */
    const auto log_submit = [&](const char* reason, int enqueued) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 32) {
            std::printf("M5 submitflip: handle=%d buf=%d arg=%lld mode=%d -> enqueued=%d reason=%s "
                        "pending=%d\n",
                        handle, bufferIndex, static_cast<long long>(flipArg), flipMode, enqueued,
                        reason, port ? port->flip_status.flip_pending_num : -1);
            std::fflush(stdout);
        }
    };
#else
    const auto log_submit = [](const char*, int) {};
#endif
    if (!port) {
        LOG_ERROR(Lib_VideoOut, "Invalid handle = {}", handle);
        log_submit("no-port", 0);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    if (flipMode != 1) {
        LOG_WARNING(Lib_VideoOut, "flipmode = {}", flipMode);
    }

    if (bufferIndex < -1 || bufferIndex > 15) {
        LOG_ERROR(Lib_VideoOut, "Invalid bufferIndex = {}", bufferIndex);
        log_submit("bad-index", 0);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_INDEX;
    }

    if (bufferIndex != -1 && port->buffer_slots[bufferIndex].group_index < 0) {
        LOG_ERROR(Lib_VideoOut, "Slot in bufferIndex = {} is not registered", bufferIndex);
        log_submit("not-registered", 0);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_INDEX;
    }

    LOG_DEBUG(Lib_VideoOut, "bufferIndex = {}, flipMode = {}, flipArg = {}", bufferIndex, flipMode,
              flipArg);

    if (!driver->SubmitFlip(port, bufferIndex, flipArg)) {
        LOG_ERROR(Lib_VideoOut, "Flip queue is full");
        log_submit("queue-full", 0);
        return ORBIS_VIDEO_OUT_ERROR_FLIP_QUEUE_FULL;
    }

    log_submit("ok", 1);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutGetEventId(const Kernel::OrbisKernelEvent* ev) {
    if (ev == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    }
    if (ev->filter != Kernel::OrbisKernelEvent::Filter::VideoOut) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT;
    }

    OrbisVideoOutInternalEventId internal_event_id =
        static_cast<OrbisVideoOutInternalEventId>(ev->ident);
    switch (internal_event_id) {
    case OrbisVideoOutInternalEventId::Flip:
        return static_cast<s32>(OrbisVideoOutEventId::Flip);
    case OrbisVideoOutInternalEventId::Vblank:
    case OrbisVideoOutInternalEventId::SysVblank:
        return static_cast<s32>(OrbisVideoOutEventId::Vblank);
    case OrbisVideoOutInternalEventId::PreVblankStart:
        return static_cast<s32>(OrbisVideoOutEventId::PreVblankStart);
    case OrbisVideoOutInternalEventId::SetMode:
        return static_cast<s32>(OrbisVideoOutEventId::SetMode);
    case OrbisVideoOutInternalEventId::Position:
        return static_cast<s32>(OrbisVideoOutEventId::Position);
    default: {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT;
    }
    }
}

s32 PS4_SYSV_ABI sceVideoOutGetEventData(const Kernel::OrbisKernelEvent* ev, s64* data) {
    if (ev == nullptr || data == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    }
    if (ev->filter != Kernel::OrbisKernelEvent::Filter::VideoOut) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT;
    }

    auto event_data = ev->data >> 0x10;
    if (ev->ident != static_cast<s32>(OrbisVideoOutInternalEventId::Flip) || ev->data >= 0) {
        *data = event_data;
    } else {
        *data = event_data | 0xffff000000000000;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutGetEventCount(const Kernel::OrbisKernelEvent* ev) {
    if (ev == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    }
    if (ev->filter != Kernel::OrbisKernelEvent::Filter::VideoOut) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_EVENT;
    }

    auto event_data = static_cast<OrbisVideoOutEventData>(ev->data);
    return event_data.count;
}

s32 PS4_SYSV_ABI sceVideoOutGetFlipStatus(s32 handle, FlipStatus* status) {
    if (!status) {
        LOG_ERROR(Lib_VideoOut, "Flip status is null");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    }

    auto* port = driver->GetPort(handle);
    if (!port) {
        LOG_ERROR(Lib_VideoOut, "Invalid port handle = {}", handle);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    {
        std::unique_lock lock{port->port_mutex};
        *status = port->flip_status;
    }

    LOG_TRACE(Lib_VideoOut,
              "count = {}, processTime = {}, tsc = {}, submitTsc = {}, flipArg = {}, gcQueueNum = "
              "{}, flipPendingNum = {}, currentBuffer = {}",
              status->count, status->process_time, status->tsc, status->submit_tsc,
              status->flip_arg, status->gc_queue_num, status->flip_pending_num,
              status->current_buffer);

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutGetVblankStatus(int handle, SceVideoOutVblankStatus* status) {
    if (status == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    }

    auto* port = driver->GetPort(handle);
    if (!port) {
        LOG_ERROR(Lib_VideoOut, "Invalid port handle = {}", handle);
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    std::unique_lock lock{port->vo_mutex};
    *status = port->vblank_status;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutGetResolutionStatus(s32 handle, SceVideoOutResolutionStatus* status) {
    LOG_INFO(Lib_VideoOut, "called");
    auto* port = driver->GetPort(handle);
    if (!port || !port->is_open) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    *status = port->resolution;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutOpen(Libraries::UserService::OrbisUserServiceUserId userId, s32 busType,
                                 s32 index, const void* param) {
    LOG_INFO(Lib_VideoOut, "called");
    ASSERT(busType == SCE_VIDEO_OUT_BUS_TYPE_MAIN);

    if (index != 0) {
        LOG_ERROR(Lib_VideoOut, "Index != 0");
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    auto* params = reinterpret_cast<const ServiceThreadParams*>(param);
    int handle = driver->Open(params);

    if (handle < 0) {
        LOG_ERROR(Lib_VideoOut, "All available handles are open");
        return ORBIS_VIDEO_OUT_ERROR_RESOURCE_BUSY;
    }

    return handle;
}

s32 PS4_SYSV_ABI sceVideoOutClose(s32 handle) {
    driver->Close(handle);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutUnregisterBuffers(s32 handle, s32 attributeIndex) {
    auto* port = driver->GetPort(handle);
    if (!port || !port->is_open) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    return driver->UnregisterBuffers(port, attributeIndex);
}

s32 PS4_SYSV_ABI sceVideoOutGetBufferLabelAddress(s32 handle, uintptr_t* label_addr) {
    if (label_addr == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    }
    auto* port = driver->GetPort(handle);
    if (!port) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }
    *label_addr = reinterpret_cast<uintptr_t>(port->buffer_labels.data());
    return 16;
}

s32 sceVideoOutSubmitEopFlip(s32 handle, u32 buf_id, u32 mode, s64 flip_arg, void** unk) {
    /* bbport: there is no driver on the contract path, and refusing the call is worse than the old
     * behaviour: the guest used the error as a pointer and faulted at rip 0x8 (immediate call through
     * 0x8). Keep the original call; the fix has to give the guest a working driver, not an error. */
    auto* port = driver->GetPort(handle);
    if (!port) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }
    /* bbport: the lambda below runs from the IRQ path and used to read the global driver, which was
     * null by then (measured: driver=0x0 at the flip IRQ, so every flip was dropped as "port gone").
     * The driver outlives the run, so capture the pointer that is valid at registration time. */
    VideoOutPort* const irq_port = port; /* bbport: the port is what the handler needs, and it outlives the run. */

#ifdef UNCHARTED_UWP
    /* bbport: this is the only flip entry the guest actually calls, and it arms a one-shot callback on
     * GfxFlip. Counting the calls next to the GfxFlip signals says which half of the cycle is missing:
     * no calls means the guest never posts a flip; calls without signals means the IRQ never fires. */
    {
        static std::atomic<int> calls{0};
        const int n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 16) {
            std::printf("M5 eopflip: call=%d handle=%d buf=%u mode=%u arg=%lld\n", n, handle, buf_id,
                        mode, static_cast<long long>(flip_arg));
            std::fflush(stdout);
        }
    }
#endif

    Platform::IrqC::Instance()->RegisterOnce(
        Platform::InterruptId::GfxFlip, [=](Platform::InterruptId irq) {
#ifdef UNCHARTED_UWP
            {
                static std::atomic<int> n{0};
                if (n.fetch_add(1) < 16) {
                    std::printf("M5 flip irq: handler entered irq=%u\n", static_cast<unsigned>(irq));
                    std::fflush(stdout);
                }
            }
            /* bbport: this lambda outlives the call and used to capture the port POINTER. Measured: a
             * read fault at 0x338 — null port plus offsetof(buffer_labels) with buf_id 0 — when the IRQ
             * ran after the port had gone. The handle is the stable identity, so resolve it here and
             * validate the index before touching either. */
            std::printf("M5 flip irq: port=%p handle=%d\n", (void*)irq_port, (int)handle);
            std::fflush(stdout);
            /* (duplicate flush removed) */
            auto* live_port = irq_port;
            if (live_port == nullptr) {
                char line[160];
                std::snprintf(line, sizeof(line),
                              "M5 flip: port gone when the flip IRQ ran (handle=%d buf_id=%d); dropped",
                              handle, buf_id);
                bb_log_raw(line);
                return;
            }
            if (buf_id < 0 || static_cast<size_t>(buf_id) >= live_port->buffer_labels.size()) {
                bb_log_raw("M5 flip: buffer index out of range; signal dropped");
                return;
            }
            ASSERT_MSG(irq == Platform::InterruptId::GfxFlip, "An unexpected IRQ occured");
            /* bbport: the flip completes here or not at all. The label says whether the guest had an
             * outstanding flip for this buffer (the hardware arms it before raising the interrupt), and
             * the result says whether the request was enqueued for the presenter. */
            {
                static std::atomic<int> n{0};
                if (n.fetch_add(1) < 16) {
                    std::printf("M5 flip irq: buf_id=%d label=%d\n", static_cast<int>(buf_id),
                                static_cast<int>(live_port->buffer_labels[buf_id]));
                    std::fflush(stdout);
                }
            }
            ASSERT_MSG(live_port->buffer_labels[buf_id] == 1, "Out of order flip IRQ");
            const auto result = driver->SubmitFlip(live_port, buf_id, flip_arg, true);
            {
                static std::atomic<int> n{0};
                if (n.fetch_add(1) < 16) {
                    std::printf("M5 flip irq: submit result=%d\n", result ? 1 : 0);
                    std::fflush(stdout);
                }
            }
            ASSERT_MSG(result, "EOP flip submission failed");
                return;
#else
            ASSERT_MSG(irq == Platform::InterruptId::GfxFlip, "An unexpected IRQ occured");
            ASSERT_MSG(port->buffer_labels[buf_id] == 1, "Out of order flip IRQ");
            const auto submit_result = driver->SubmitFlip(port, buf_id, flip_arg, true);
            ASSERT_MSG(submit_result, "EOP flip submission failed");
#endif
        });

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutGetDeviceCapabilityInfo(
    s32 handle, SceVideoOutDeviceCapabilityInfo* pDeviceCapabilityInfo) {
    pDeviceCapabilityInfo->capability = 0;
    if (presenter->IsHDRSupported()) {
        auto& game_info = Common::ElfInfo::Instance();
        if (game_info.GetPSFAttributes().support_hdr) {
            pDeviceCapabilityInfo->capability |= ORBIS_VIDEO_OUT_DEVICE_CAPABILITY_BT2020_PQ;
        }
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutWaitVblank(s32 handle) {
    auto* port = driver->GetPort(handle);
    if (!port) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    std::unique_lock lock{port->vo_mutex};
    const auto prev_counter = port->vblank_status.count;
    port->vblank_cv.wait(lock, [&]() { return prev_counter != port->vblank_status.count; });
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutColorSettingsSetGamma(SceVideoOutColorSettings* settings, float gamma) {
    if (gamma < 0.1f || gamma > 2.0f) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }
    settings->gamma = gamma;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutAdjustColor(s32 handle, const SceVideoOutColorSettings* settings) {
    if (settings == nullptr) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_ADDRESS;
    }

    auto* port = driver->GetPort(handle);
    if (!port) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    presenter->GetPPSettingsRef().gamma = settings->gamma;
    return ORBIS_OK;
}

struct Mode {
    u32 size;
    u8 encoding;
    u8 range;
    u8 colorimetry;
    u8 depth;
    u64 refresh_rate;
    u64 resolution;
    u8 reserved[8];
};

void PS4_SYSV_ABI sceVideoOutModeSetAny_(Mode* mode, u32 size) {
    std::memset(mode, 0xff, size);
    mode->size = size;
}

s32 PS4_SYSV_ABI sceVideoOutConfigureOutputMode_(s32 handle, u32 reserved, const Mode* mode,
                                                 const void* options, u32 size_mode,
                                                 u32 size_options) {
    auto* port = driver->GetPort(handle);
    if (!port) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    if (reserved != 0) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    switch (mode->colorimetry) {
    case OrbisVideoOutColorimetry::Any:
        port->is_hdr = false;
        break;
    case OrbisVideoOutColorimetry::Bt2020PQ:
        if (Common::ElfInfo::Instance().GetPSFAttributes().support_hdr) {
            port->is_hdr = true;
        }
        break;
    default:
        return ORBIS_VIDEO_OUT_ERROR_INVALID_VALUE;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceVideoOutSubmitChangeBufferAttribute(s32 handle, s32 attributeIndex,
                                                        const BufferAttribute* attribute) {
    auto* port = driver->GetPort(handle);
    if (!port || !port->is_open) {
        return ORBIS_VIDEO_OUT_ERROR_INVALID_HANDLE;
    }

    return driver->ChangeBufferAttribute(port, attributeIndex, attribute);
}

s32 PS4_SYSV_ABI sceVideoOutSetWindowModeMargins(s32 handle, s32 top, s32 bottom) {
    LOG_ERROR(Lib_VideoOut, "(STUBBED) called top = {}, bottom = {}", top, bottom);
    return ORBIS_OK;
}

#ifdef UNCHARTED_UWP
/* bbport: the contract registers VideoOut entry points without ever creating the driver, so every flip
 * resolves a null driver (measured: port=0x188). This creates only the driver; the real entry points stay
 * unregistered on purpose, because registering them crashes here (access violation at 0x79e8). */
bool DriverAlive() {
    return driver != nullptr;
}

/* Forward for the UWP contract: the flip event has to be registered on the real port, because that is
 * the list the flip completion walks. */
s32 AddFlipEventForward(Kernel::OrbisKernelEqueue queue, s32 handle, void* udata) {
    return sceVideoOutAddFlipEvent(queue, handle, udata);
}

void EnsureDriver() {
    if (driver == nullptr) {
        driver = std::make_unique<VideoOutDriver>(EmulatorSettings.GetInternalScreenWidth(),
                                                 EmulatorSettings.GetInternalScreenHeight());
        std::printf("M5 videoout: driver created (UWP contract path)\n");
        std::fflush(stdout);
    }
}
#endif

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    {
        static std::atomic<int> n{0};
        const int c = n.fetch_add(1, std::memory_order_relaxed) + 1;
        std::printf("M5 videoout: RegisterLib call=%d driver_before=%p\n", c, (void*)driver.get());
        std::fflush(stdout);
    }
    driver = std::make_unique<VideoOutDriver>(EmulatorSettings.GetInternalScreenWidth(),
                                              EmulatorSettings.GetInternalScreenHeight());

#ifdef UNCHARTED_UWP
    BbSetFlipPendingProbe([]() -> int {
        return driver ? driver->GetPort(1)->flip_status.flip_pending_num : 0;
    });
#endif

    LIB_FUNCTION("SbU3dwp80lQ", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutGetFlipStatus);
    LIB_FUNCTION("U46NwOiJpys", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutSubmitFlip);
    LIB_FUNCTION("w3BY+tAEiQY", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutRegisterBuffers);
    LIB_FUNCTION("HXzjK9yI30k", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutAddFlipEvent);
    LIB_FUNCTION("Xru92wHJRmg", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutAddVblankEvent);
    LIB_FUNCTION("CBiu4mCE1DA", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutSetFlipRate);
    LIB_FUNCTION("i6-sR91Wt-4", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutSetBufferAttribute);
    LIB_FUNCTION("6kPnj51T62Y", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutGetResolutionStatus);
    LIB_FUNCTION("8XGijEoThE0", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutGetResolutionStatus); // sceVideoOutSysGetResolutionStatus
    LIB_FUNCTION("Ek+VR4lcJQI", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutAddVblankEvent); // sceVideoOutSysAddVblankEvent
    LIB_FUNCTION("Up36PTk687E", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutOpen);
    LIB_FUNCTION("zgXifHT9ErY", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutIsFlipPending);
    LIB_FUNCTION("N5KDtkIjjJ4", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutUnregisterBuffers);
    LIB_FUNCTION("OcQybQejHEY", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutGetBufferLabelAddress);
    LIB_FUNCTION("uquVH4-Du78", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutClose);
    LIB_FUNCTION("1FZBKy8HeNU", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutGetVblankStatus);
    LIB_FUNCTION("d1AjT2uZJn0", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutGetVblankStatus);
    LIB_FUNCTION("kGVLc3htQE8", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutGetDeviceCapabilityInfo);
    LIB_FUNCTION("j6RaAUlaLv0", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutWaitVblank);
    LIB_FUNCTION("U2JJtSqNKZI", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutGetEventId);
    LIB_FUNCTION("rWUTcKdkUzQ", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutGetEventData);
    LIB_FUNCTION("Mt4QHHkxkOc", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutGetEventCount);
    LIB_FUNCTION("DYhhWbJSeRg", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutColorSettingsSetGamma);
    LIB_FUNCTION("pv9CI5VC+R0", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutAdjustColor);
    LIB_FUNCTION("-Ozn0F1AFRg", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutDeleteFlipEvent);
    LIB_FUNCTION("oNOQn3knW6s", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutDeleteVblankEvent);
    LIB_FUNCTION("pjkDsgxli6c", "libSceVideoOut", 1, "libSceVideoOut", sceVideoOutModeSetAny_);
    LIB_FUNCTION("N1bEoJ4SRw4", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutConfigureOutputMode_);
    LIB_FUNCTION("MTxxrOCeSig", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutSetWindowModeMargins);
    LIB_FUNCTION("IOdgHlCGU-k", "libSceVideoOut", 1, "libSceVideoOut",
                 sceVideoOutSubmitChangeBufferAttribute);
}

} // namespace Libraries::VideoOut
