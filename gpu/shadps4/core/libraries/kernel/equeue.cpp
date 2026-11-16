// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <thread>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/kernel/posix_error.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/libs.h"

#ifdef UNCHARTED_M5_VIDEO_CONTRACT
extern "C" void bbgpu_uwp_gnm_event_cleanup(Libraries::Kernel::OrbisKernelEqueue eq);
extern "C" void bbgpu_uwp_videoout_event_cleanup(Libraries::Kernel::OrbisKernelEqueue eq);
#endif

namespace Libraries::Kernel {

extern boost::asio::io_context io_context;
extern void KernelSignalRequest();

// bbport: the queue table is used from every guest thread (create, wait, trigger, timers). A
// lookup racing an insert (the map rehashing) read freed memory: Uncharted 2 crashed at start
// while its GPU interrupt threads created and waited on queues. Lookups share a lock; values
// are node-based, so references stay valid after the lock is released.
class EqueueTable {
public:
    bool contains(s32 handle) const {
        std::shared_lock lock{mutex};
        return map.contains(handle);
    }
    EqueueInternal*& operator[](s32 handle) {
        {
            std::shared_lock lock{mutex};
            if (const auto it = map.find(handle); it != map.end()) {
                return it->second;
            }
        }
        std::unique_lock lock{mutex};
        return map[handle];
    }
    void erase(s32 handle) {
        std::unique_lock lock{mutex};
        map.erase(handle);
    }

#ifdef UNCHARTED_UWP
    /* bbport: the clock needs to walk the queues while they are being used, and it must not hold the
     * table lock while triggering (TriggerEvent takes the queue's own mutex). */
    template <typename Fn>
    void ForEach(Fn&& fn) {
        std::shared_lock lock{mutex};
        for (auto& [handle, queue] : map) {
            if (queue != nullptr) {
                fn(handle, queue);
            }
        }
    }
#endif

private:
    std::unordered_map<s32, EqueueInternal*> map;
    mutable std::shared_mutex mutex;
};
static EqueueTable kqueues;
static constexpr auto HrTimerSpinlockThresholdNs = 1200000u;

EqueueInternal* GetEqueue(OrbisKernelEqueue eq) {
    if (!kqueues.contains(eq)) {
        return nullptr;
    }
    return kqueues[eq];
}

static void HrTimerCallback(OrbisKernelEqueue eq, const OrbisKernelEvent& kevent) {
    if (kqueues.contains(eq)) {
        kqueues[eq]->TriggerEvent(kevent.ident, OrbisKernelEvent::Filter::HrTimer, kevent.udata);
    }
}

static void TimerCallback(OrbisKernelEqueue eq, const OrbisKernelEvent& kevent) {
    if (kqueues.contains(eq) && kqueues[eq]->EventExists(kevent.ident, kevent.filter)) {
        kqueues[eq]->TriggerEvent(kevent.ident, OrbisKernelEvent::Filter::Timer, kevent.udata);
        if (!(kevent.flags & OrbisKernelEvent::Flags::OneShot)) {
            // Reschedule the event for its next period.
            kqueues[eq]->ScheduleEvent(kevent.ident, kevent.filter, TimerCallback);
        }
    }
}

// Events are uniquely identified by id and filter.
bool EqueueInternal::AddEvent(EqueueEvent& event) {
    // Save id and filter before event is moved into m_events.
    const u64 id = event.event.ident;
    const auto filter = event.event.filter;

#ifdef UNCHARTED_UWP
    /* bbport: every registration goes through here, whatever the API the guest used. It answers the
     * question that decides who may signal: a flip registration owns its payload, a user event does not. */
    {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 32) {
            std::printf("M5 equeue reg: name=%s ident=%llu filter=%d udata=%p\n",
                        std::string(m_name).c_str(), static_cast<unsigned long long>(id),
                        static_cast<int>(filter), event.event.udata);
            std::fflush(stdout);
        }
    }
#endif

    {
        std::scoped_lock lock{m_mutex};

        // Calculate timer interval
        event.time_added = std::chrono::steady_clock::now();
        if (event.event.filter == OrbisKernelEvent::Filter::Timer) {
            // Set timer interval, this is stored in milliseconds for timers.
            event.timer_interval = std::chrono::milliseconds(event.event.data);
        } else if (event.event.filter == OrbisKernelEvent::Filter::HrTimer) {
            // Retrieve inputted time, this is stored in the bintime format.
            OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(event.event.data);

            // Convert the bintime format to a timespec.
            OrbisKernelTimespec ts;
            ts.tv_sec = time->sec;
            ts.tv_nsec = (1000000000 * (time->frac >> 32)) >> 32;

            // Then use the timespec to set the timer interval.
            event.timer_interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
        }

        // First, check if there's already an event with the same id and filter.
        const auto& find_it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
            return ev.event.ident == id && ev.event.filter == filter;
        });
        // If there is a duplicate event, we need to update that instead.
        if (find_it != m_events.cend()) {
            // Specifically, update user data and timer_interval.
            // Trigger status and event data should remain intact.
            auto& old_event = *find_it;
            old_event.timer_interval = event.timer_interval;
            old_event.event.udata = event.event.udata;
            return true;
        }

        // Clear input data from event.
        event.event.data = 0;

        // Remove add flag from event
        event.event.flags &= ~OrbisKernelEvent::Flags::Add;

        // Clear flag is appended to most event types internally.
        if (event.event.filter != OrbisKernelEvent::Filter::User) {
            event.event.flags |= OrbisKernelEvent::Flags::Clear;
        }

        const auto& it = std::ranges::find(m_events, event);
        if (it != m_events.cend()) {
            *it = std::move(event);
        } else {
            m_events.emplace_back(std::move(event));
        }
    }

    // Schedule callbacks for timer events
    if (filter == OrbisKernelEvent::Filter::Timer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::Timer, TimerCallback);
    } else if (filter == OrbisKernelEvent::Filter::HrTimer) {
        return this->ScheduleEvent(id, OrbisKernelEvent::Filter::HrTimer, HrTimerCallback);
    }

    return true;
}

bool EqueueInternal::ScheduleEvent(u64 id, s16 filter,
                                   void (*callback)(OrbisKernelEqueue, const OrbisKernelEvent&)) {
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it == m_events.cend()) {
        return false;
    }

    const auto& event = *it;
    ASSERT(event.event.filter == OrbisKernelEvent::Filter::Timer ||
           event.event.filter == OrbisKernelEvent::Filter::HrTimer);

    if (!it->timer) {
        it->timer = std::make_unique<boost::asio::steady_timer>(io_context, event.timer_interval);
    } else {
        // If the timer already exists we are scheduling a reoccurrence after the next period.
        // Set the expiration time to the previous occurrence plus the period.
        it->timer->expires_at(it->timer->expiry() + event.timer_interval);
    }

    it->timer->async_wait(
        [this, event_data = event.event, callback](const boost::system::error_code& ec) {
            if (ec) {
                if (ec != boost::system::errc::operation_canceled) {
                    LOG_ERROR(Kernel_Event, "Timer callback error: {}", ec.message());
                } else {
                    // Timer was cancelled (removed) before it triggered
                    LOG_DEBUG(Kernel_Event, "Timer cancelled");
                }
                return;
            }
            callback(this->m_handle, event_data);
        });
    KernelSignalRequest();

    return true;
}

bool EqueueInternal::RemoveEvent(u64 id, s16 filter) {
    bool has_found = false;
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });
    if (it != m_events.cend()) {
        m_events.erase(it);
        has_found = true;
    }
    return has_found;
}

int EqueueInternal::WaitForEvents(OrbisKernelEvent* ev, int num, const OrbisKernelUseconds* timo) {
    if (timo != nullptr && *timo == 0) {
        // Effectively acts as a poll; only events that have already
        // arrived at the time of this function call can be received
        return GetTriggeredEvents(ev, num);
    }
    const auto micros = timo ? *timo : 0u;

    if (HasSmallTimer()) {
        // If a small timer is set, just wait for it to expire.
        return WaitForSmallTimer(ev, num, micros);
    }

    int count = 0;

    const auto predicate = [&] {
        count = GetTriggeredEvents(ev, num);
        return count > 0;
    };

    if (micros == 0) {
        // Wait indefinitely for events
        std::unique_lock lock{m_mutex};
#ifdef UNCHARTED_UWP
        m_waiters.fetch_add(1, std::memory_order_relaxed);
#endif
        m_cond.wait(lock, predicate);
#ifdef UNCHARTED_UWP
        m_waiters.fetch_sub(1, std::memory_order_relaxed);
#endif
    } else {
        // Wait up until the timeout value
        std::unique_lock lock{m_mutex};
        m_cond.wait_for(lock, std::chrono::microseconds(micros), predicate);
    }

    return count;
}

#ifdef UNCHARTED_UWP
#ifdef UNCHARTED_UWP
/* bbport: how many flips the guest has submitted and not yet seen completed. Provided by the video out
 * driver, because a flip event without a pending flip is fiction: the guest's handler runs and uses state
 * that does not exist (measured: faults at rip 0x9 and 0x914b5e with dcb 0). */
std::atomic<int (*)(void)> g_flip_pending_probe{nullptr};
extern "C" void BbSetFlipPendingProbe(int (*probe)(void)) {
    g_flip_pending_probe.store(probe, std::memory_order_release);
}
static int PendingFlips() {
    const auto probe = g_flip_pending_probe.load(std::memory_order_acquire);
    return probe != nullptr ? probe() : 0;
}
#endif

void EqueueInternal::SignalOne() {    /* Wake exactly one blocked caller with the FLIP event only (ident 6, filter -13, no payload). The
     * registrations measured on this title are: FLIP QUEUE ident=6 filter=-13 and GPU NdJob Sync Queue
     * ident=64 filter=-14. The second is a user event whose payload belongs to the guest's own producer,
     * and triggering it from here corrupted the guest (fault at rip 0x9). The flip carries no payload,
     * so zero is the correct data for it. */
    constexpr u64 kFlipIdent = 6;
    constexpr s16 kFlipFilter = -13;
    TriggerEvent(kFlipIdent, kFlipFilter, nullptr);
}

void StartPresentClock() {
    /* bbport: a source of time that does not depend on the guest submitting. Measured problem: nine
     * deadline-free waits (eight threads on the GPU job sync queue, one on the flip queue) against a
     * single completion, because every signal in the port was produced by a submission that the guest
     * was itself waiting for.
     *
     * OFF by default: signalling a registered event with no payload is not safe - measured, the guest
     * faults at rip 0x9 (it calls a pointer it expected the event data to carry). The clock only makes
     * sense once each event is delivered with the payload the guest registered for it. */
    static const bool enabled = [] {
        const char* env = std::getenv("BB_PRESENT_CLOCK");
        return env != nullptr && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            Common::SetCurrentThreadName("bb:PresentClock");
            unsigned tick = 0;
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                ++tick;
                unsigned emitted = 0;
                unsigned waiters = 0;
                kqueues.ForEach([&](s32, EqueueInternal* queue) {
                    const int pending = queue->PendingWaiters();
                    if (pending > 0) {
                        waiters += static_cast<unsigned>(pending);
                        /* Only a flip the guest actually submitted may be completed, and only on the
                         * flip queue: that keeps the hardware meaning (one completion per submitted
                         * flip) and never delivers an event without the state behind it. */
                        if (std::string_view(queue->GetName()).find("FLIP") != std::string_view::npos &&
                            PendingFlips() > 0) {
                            queue->SignalOne();
                            ++emitted;
                        }
                    }
                });
                if ((tick % 120) == 0 && waiters > 0) {
                    std::printf("M5 clock: tick=%u emitted=%u waiters=%u\n", tick, emitted, waiters);
                    std::fflush(stdout);
                }
            }
        }).detach();
    });
}
#endif

bool EqueueInternal::TriggerEvent(u64 ident, s16 filter, void* trigger_data) {    bool has_found = false;
    {
        std::scoped_lock lock{m_mutex};
        for (auto& event : m_events) {
            if (event.event.ident == ident && event.event.filter == filter) {
                if (filter == OrbisKernelEvent::Filter::VideoOut) {
                    event.TriggerDisplay(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::User) {
                    event.TriggerUser(trigger_data);
                } else if (filter == OrbisKernelEvent::Filter::Timer ||
                           filter == OrbisKernelEvent::Filter::HrTimer) {
                    event.TriggerTimer();
                } else {
                    event.Trigger(trigger_data);
                }
                has_found = true;
            }
        }
    }
    m_cond.notify_one();
#ifdef UNCHARTED_UWP
    /* bbport: the waiter count AT THE INSTANT of the trigger is the whole question: an event delivered
     * while nobody waits is an event lost (the guest will wait for it later and never be told), while a
     * zero "found" means the guest never registered that ident on this queue. */
    {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 64) {
            std::printf("M5 trigger: name=%s ident=%llu filter=%d waiters=%d found=%d caller=%p\n",
                        std::string(m_name).c_str(), static_cast<unsigned long long>(ident),
                        static_cast<int>(filter), m_waiters.load(std::memory_order_relaxed),
                        has_found ? 1 : 0, __builtin_return_address(0));
            std::fflush(stdout);
        }
    }
#endif
    return has_found;
}

int EqueueInternal::GetTriggeredEvents(OrbisKernelEvent* ev, int num) {
    int count = 0;
    for (auto it = m_events.begin(); it != m_events.end();) {
        if (it->IsTriggered()) {
            ev[count++] = it->event;
            if (it->event.flags & OrbisKernelEvent::Flags::Clear) {
                it->Clear();
            }
            if (it->event.flags & OrbisKernelEvent::Flags::OneShot) {
                it = m_events.erase(it);
            } else {
                ++it;
            }

            if (count == num) {
                break;
            }
        } else {
            ++it;
        }
    }

    return count;
}

bool EqueueInternal::AddSmallTimer(EqueueEvent& ev) {
    // Retrieve inputted time, this is stored in the bintime format
    OrbisKernelBintime* time = reinterpret_cast<OrbisKernelBintime*>(ev.event.data);
    OrbisKernelTimespec ts;
    ts.tv_sec = time->sec;
    ts.tv_nsec = ((1000000000 * (time->frac >> 32)) >> 32);

    // Create the small timer
    SmallTimer st;
    st.event = ev.event;
    st.added = std::chrono::steady_clock::now();
    st.interval = std::chrono::nanoseconds(ts.tv_nsec + ts.tv_sec * 1000000000);
    {
        std::scoped_lock lock{m_mutex};
        m_small_timers[st.event.ident] = std::move(st);
    }
    return true;
}

int EqueueInternal::WaitForSmallTimer(OrbisKernelEvent* ev, int num, u32 micros) {
    ASSERT(num >= 1);

    auto curr_clock = std::chrono::steady_clock::now();
    const auto wait_end_us = (micros == 0) ? std::chrono::steady_clock::time_point::max()
                                           : curr_clock + std::chrono::microseconds{micros};
    int count = 0;
    do {
        curr_clock = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock{m_mutex};
            for (auto it = m_small_timers.begin(); it != m_small_timers.end() && count < num;) {
                const SmallTimer& st = it->second;

                if (curr_clock - st.added >= st.interval) {
                    ev[count++] = st.event;
                    it = m_small_timers.erase(it);
                } else {
                    ++it;
                }
            }

            if (count > 0)
                return count;
        }
        std::this_thread::yield();
    } while (curr_clock < wait_end_us);

    return 0;
}

bool EqueueInternal::EventExists(u64 id, s16 filter) {
    std::scoped_lock lock{m_mutex};

    const auto& it = std::ranges::find_if(m_events, [id, filter](auto& ev) {
        return ev.event.ident == id && ev.event.filter == filter;
    });

    return it != m_events.cend();
}

static s32 NextEqueueHandle() { // bbport
    static std::atomic<s32> next{0x40000};
    return next++;
}

s32 PS4_SYSV_ABI posix_kqueue() {
    // Reserve a file handle for the kqueue
    // bbport: equeue handles come from their own range, not the file table.
    s32 kqueue_handle = NextEqueueHandle();

    // Plenty of equeue logic uses names to identify queues.
    // Create a unique name for the queue we create.
    char name[32];
    memset(name, 0, sizeof(name));
    snprintf(name, sizeof(name), "kqueue%i", kqueue_handle);

    // Create the queue
    kqueues[kqueue_handle] = new EqueueInternal(kqueue_handle, name);
    LOG_INFO(Kernel_Event, "kqueue created with name {}", name);

    // Return handle.
    return kqueue_handle;
}

// Helper method to detect supported filters.
// We don't want to allow adding events we don't handle properly.
bool SupportedEqueueFilter(OrbisKernelEvent::Filter filter) {
    return filter == OrbisKernelEvent::Filter::GraphicsCore ||
           filter == OrbisKernelEvent::Filter::HrTimer ||
           filter == OrbisKernelEvent::Filter::Timer || filter == OrbisKernelEvent::Filter::User ||
           filter == OrbisKernelEvent::Filter::VideoOut;
}

s32 PS4_SYSV_ABI posix_kevent(s32 handle, OrbisKernelEvent* changelist, u64 nchanges,
                              OrbisKernelEvent* eventlist, u64 nevents,
                              OrbisKernelTimespec* timeout) {
    LOG_INFO(Kernel_Event, "called, eq = {}, nchanges = {}, nevents = {}", handle, nchanges,
             nevents);

    // Get the equeue
    if (!kqueues.contains(handle)) {
        *__Error() = POSIX_EBADF;
        return ORBIS_FAIL;
    }
    auto equeue = kqueues[handle];

    // First step is to apply all changes in changelist.
    for (u64 i = 0; i < nchanges; i++) {
        auto event = changelist[i];
        if (!SupportedEqueueFilter(event.filter)) {
            LOG_ERROR(Kernel_Event, "Unsupported event filter {}",
                      magic_enum::enum_name(event.filter));
            continue;
        }

        // Check the event flags to determine the appropriate action
        if (event.flags & OrbisKernelEvent::Flags::Add) {
            // The caller is requesting to add an event.
            EqueueEvent internal_event{};
            internal_event.event = event;
            if (!equeue->AddEvent(internal_event)) {
                // Failed to add event, return error.
                *__Error() = POSIX_ENOMEM;
                return ORBIS_FAIL;
            }
        }

        if (event.flags & OrbisKernelEvent::Flags::Delete) {
            // The caller is requesting to remove an event.
            if (!equeue->RemoveEvent(event.ident, event.filter)) {
                // Failed to remove event, return error.
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        }

        if (event.filter == OrbisKernelEvent::Filter::User && event.fflags == 0x1000000) {
            // For user events, this fflags value indicates we need to trigger the event.
            if (!equeue->TriggerEvent(event.ident, OrbisKernelEvent::Filter::User, event.udata)) {
                *__Error() = POSIX_ENOENT;
                return ORBIS_FAIL;
            }
        } else if (event.fflags != 0) {
            // The title is using filter-specific flags. Right now, these are unhandled.
            LOG_ERROR(Kernel_Event, "Unhandled fflags {:#x} for event filter {}", event.fflags,
                      magic_enum::enum_name(event.filter));
            continue;
        }
    }

    // Now we need to wait on the event list.
    s32 count = 0;
    if (nevents > 0) {
        if (timeout != nullptr) {
            OrbisKernelUseconds micros = (timeout->tv_sec * 1000000) + (timeout->tv_nsec / 1000);
            count = equeue->WaitForEvents(eventlist, nevents, &micros);
        } else {
            count = equeue->WaitForEvents(eventlist, nevents, nullptr);
        }
    }
    return count;
}

int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name) {
    if (eq == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    if (name == nullptr) {
        LOG_ERROR(Kernel_Event, "Event queue name is null!");
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Maximum is 32 including null terminator
    static constexpr u64 MaxEventQueueNameSize = 32;
    if (std::strlen(name) > MaxEventQueueNameSize) {
        LOG_ERROR(Kernel_Event, "Event queue name exceeds 32 bytes!");
        return ORBIS_KERNEL_ERROR_ENAMETOOLONG;
    }

    LOG_INFO(Kernel_Event, "name = {}", name);

    // Reserve a file handle for the kqueue
    // bbport: equeue handles come from their own range, not the file table.
    OrbisKernelEqueue kqueue_handle = NextEqueueHandle();

    // Create the equeue
    kqueues[kqueue_handle] = new EqueueInternal(kqueue_handle, name);
    *eq = kqueue_handle;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue eq) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

#ifdef UNCHARTED_M5_VIDEO_CONTRACT
    // Remove GPU/VideoOut observers before the queue storage is freed. Without this,
    // a later IRQ can call an EqueueInternal* that points into freed memory.
    bbgpu_uwp_gnm_event_cleanup(eq);
    bbgpu_uwp_videoout_event_cleanup(eq);
#endif
    delete kqueues[eq];
    kqueues.erase(eq);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                                     OrbisKernelUseconds* timo) {
    HLE_TRACE;
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    auto& equeue = kqueues[eq];

    TRACE_HINT(equeue->GetName());
    LOG_TRACE(Kernel_Event, "equeue = {} num = {}", equeue->GetName(), num);

    if (ev == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }

    if (num < 1) {
        *out = 0;
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

#ifdef UNCHARTED_UWP
    const auto wait_begin = std::chrono::steady_clock::now();
    /* A wait with no deadline never returns if nobody signals it, and a stalled run looks exactly like a
     * working one. Logging the entry means a freeze leaves an unmatched "wait begin" behind, which names
     * the queue the guest is stuck on. */
    if (timo == nullptr || *timo == 0) {
        /* The thread name says whether the blocked caller is the render/present path or a worker. */
        char thread_name[64]{};
        PWSTR wide = nullptr;
        if (SUCCEEDED(::GetThreadDescription(::GetCurrentThread(), &wide)) && wide) {
            ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, thread_name, sizeof(thread_name), nullptr,
                                  nullptr);
            ::LocalFree(wide);
        }
        std::printf("M5 wait begin: equeue=%s tid=%lu thread=%s\n",
                    std::string(equeue->GetName()).c_str(),
                    static_cast<unsigned long>(GetCurrentThreadId()),
                    thread_name[0] ? thread_name : "?");
        std::fflush(stdout);
    }
#endif
    *out = equeue->WaitForEvents(ev, num, timo);

#ifdef UNCHARTED_UWP
    /* bbport: when the run stops making progress, the question is which wait the guest is sitting on.
     * The equeue name identifies its owner (video out, gnm, ...), so a long wait names the blocker. */
    {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - wait_begin);
        if (waited.count() >= 200) {
            /* Every wait on this queue is worth seeing: the ORDER of the durations separates "the first
             * one pays for pipeline compilation" from "every one of them waits for a signal that only
             * arrives after our own init reaches the first flip". */
            static std::atomic<int> logged{0};
            const int index = logged.fetch_add(1);
            if (index < 400) {
                std::printf("M5 wait #%d: equeue=%s took=%llu ms events=%d timeout_us=%u ident=%d tid=%lu\n",
                            index, std::string(equeue->GetName()).c_str(),
                            static_cast<unsigned long long>(waited.count()), static_cast<int>(*out),
                            timo ? *timo : 0u, static_cast<int>(ev->ident),
                            static_cast<unsigned long>(GetCurrentThreadId()));
                std::fflush(stdout);
            }
        }
    }
#endif

    if (*out == 0) {
        return ORBIS_KERNEL_ERROR_ETIMEDOUT;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceKernelAddHRTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelTimespec* ts,
                                          void* udata) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    const auto total_ns = ts->tv_sec * 1000000000 + ts->tv_nsec;

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::HrTimer;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::OneShot;
    event.event.fflags = 0;
    // Data is stored as the address of a OrbisKernelBintime struct.
    OrbisKernelBintime time{ts->tv_sec, ts->tv_nsec * 0x44b82fa09};
    event.event.data = reinterpret_cast<u64>(&time);
    event.event.udata = udata;

    // HR timers cannot be implemented within the existing event queue architecture due to the
    // slowness of the notification mechanism. For instance, a 100us timer will lose its precision
    // as the trigger time drifts by +50-700%, depending on the host PC and workload. To address
    // this issue, we use a spinlock for small waits (which can be adjusted using
    // `HrTimerSpinlockThresholdUs`) and fall back to boost asio timers if the time to tick is
    // large. Even for large delays, we truncate a small portion to complete the wait
    // using the spinlock, prioritizing precision.
    auto& equeue = kqueues[eq];
    if (total_ns < HrTimerSpinlockThresholdNs) {
        return equeue->AddSmallTimer(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
    }

    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteHRTimerEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    auto& equeue = kqueues[eq];
    if (equeue->HasSmallTimer()) {
        return equeue->RemoveSmallTimer(id) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOENT;
    } else {
        return equeue->RemoveEvent(id, OrbisKernelEvent::Filter::HrTimer)
                   ? ORBIS_OK
                   : ORBIS_KERNEL_ERROR_ENOENT;
    }
}

int PS4_SYSV_ABI sceKernelAddTimerEvent(OrbisKernelEqueue eq, int id, OrbisKernelUseconds usec,
                                        void* udata) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = static_cast<u64>(id);
    event.event.filter = OrbisKernelEvent::Filter::Timer;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = usec / 1000;
    event.event.udata = udata;

    auto& equeue = kqueues[eq];
    if (!equeue->AddEvent(event)) {
        return ORBIS_KERNEL_ERROR_ENOMEM;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteTimerEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    return kqueues[eq]->RemoveEvent(id, OrbisKernelEvent::Filter::Timer)
               ? ORBIS_OK
               : ORBIS_KERNEL_ERROR_ENOENT;
}

int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add;
    event.event.fflags = 0;
    event.event.data = 0;

#ifdef UNCHARTED_UWP
    /* bbport: which registration owns the ident decides who may signal it. A user event carries no
     * payload at registration - the guest's producer supplies it - so triggering it from the port is
     * exactly what corrupted the guest (rip 0x9). */
    std::printf("M5 equeue reg: eq=%p name=%s ident=%d type=user data=0\n", eq,
                std::string(kqueues[eq]->GetName()).c_str(), id);
    std::fflush(stdout);
#endif

    return kqueues[eq]->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

int PS4_SYSV_ABI sceKernelAddUserEventEdge(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    EqueueEvent event{};
    event.event.ident = id;
    event.event.filter = OrbisKernelEvent::Filter::User;
    event.event.udata = 0;
    event.event.flags = OrbisKernelEvent::Flags::Add | OrbisKernelEvent::Flags::Clear;
    event.event.fflags = 0;
    event.event.data = 0;

    return kqueues[eq]->AddEvent(event) ? ORBIS_OK : ORBIS_KERNEL_ERROR_ENOMEM;
}

void* PS4_SYSV_ABI sceKernelGetEventUserData(const OrbisKernelEvent* ev) {
    ASSERT(ev);
    return ev->udata;
}

u64 PS4_SYSV_ABI sceKernelGetEventId(const OrbisKernelEvent* ev) {
    return ev->ident;
}

int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue eq, int id, void* udata) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

#ifdef UNCHARTED_UWP
    /* bbport: a user event belongs to the guest's own producer, so this call is the proof that the
     * guest's job-completion path runs at all. If it never appears for ident 64, the signal is missing
     * inside the guest, not in the port. */
    {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 48) {
            std::printf("M5 trigger user: name=%s ident=%d udata=%p\n",
                        std::string(kqueues[eq]->GetName()).c_str(), id, udata);
            std::fflush(stdout);
        }
    }
#endif

    if (!kqueues[eq]->TriggerEvent(id, OrbisKernelEvent::Filter::User, udata)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue eq, int id) {
    if (!kqueues.contains(eq)) {
        return ORBIS_KERNEL_ERROR_EBADF;
    }

    if (!kqueues[eq]->RemoveEvent(id, OrbisKernelEvent::Filter::User)) {
        return ORBIS_KERNEL_ERROR_ENOENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceKernelGetEventFilter(const OrbisKernelEvent* ev) {
    return ev->filter;
}

u64 PS4_SYSV_ABI sceKernelGetEventData(const OrbisKernelEvent* ev) {
    return ev->data;
}

void RegisterEventQueue(Core::Loader::SymbolsResolver* sym) {
#ifdef UNCHARTED_UWP
    StartPresentClock();
#endif
    LIB_FUNCTION("nh2IFMgKTv8", "libScePosix", 1, "libkernel", posix_kqueue);
    LIB_FUNCTION("RW-GEfpnsqg", "libScePosix", 1, "libkernel", posix_kevent);
    LIB_FUNCTION("D0OdFMjp46I", "libkernel", 1, "libkernel", sceKernelCreateEqueue);
    LIB_FUNCTION("jpFjmgAC5AE", "libkernel", 1, "libkernel", sceKernelDeleteEqueue);
    LIB_FUNCTION("fzyMKs9kim0", "libkernel", 1, "libkernel", sceKernelWaitEqueue);
    LIB_FUNCTION("vz+pg2zdopI", "libkernel", 1, "libkernel", sceKernelGetEventUserData);
    LIB_FUNCTION("4R6-OvI2cEA", "libkernel", 1, "libkernel", sceKernelAddUserEvent);
    LIB_FUNCTION("WDszmSbWuDk", "libkernel", 1, "libkernel", sceKernelAddUserEventEdge);
    LIB_FUNCTION("R74tt43xP6k", "libkernel", 1, "libkernel", sceKernelAddHRTimerEvent);
    LIB_FUNCTION("J+LF6LwObXU", "libkernel", 1, "libkernel", sceKernelDeleteHRTimerEvent);
    LIB_FUNCTION("57ZK+ODEXWY", "libkernel", 1, "libkernel", sceKernelAddTimerEvent);
    LIB_FUNCTION("YWQFUyXIVdU", "libkernel", 1, "libkernel", sceKernelDeleteTimerEvent);
    LIB_FUNCTION("F6e0kwo4cnk", "libkernel", 1, "libkernel", sceKernelTriggerUserEvent);
    LIB_FUNCTION("LJDwdSNTnDg", "libkernel", 1, "libkernel", sceKernelDeleteUserEvent);
    LIB_FUNCTION("mJ7aghmgvfc", "libkernel", 1, "libkernel", sceKernelGetEventId);
    LIB_FUNCTION("23CPPI1tyBY", "libkernel", 1, "libkernel", sceKernelGetEventFilter);
    LIB_FUNCTION("kwGyyjohI50", "libkernel", 1, "libkernel", sceKernelGetEventData);
}

} // namespace Libraries::Kernel
