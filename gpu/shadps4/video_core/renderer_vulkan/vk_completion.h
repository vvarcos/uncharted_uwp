// bbport: completion domains and submit scopes.
//
// The staging ring used to decide reuse from "was this block reserved in the current tick?" plus a
// heuristic for the case where nothing had ever been submitted. That heuristic was wrong in the
// direction that matters: a block reserved while a command buffer is being recorded may already be
// referenced by that command buffer, and reusing it before the submit lets the GPU read overwritten
// bytes. The measured GPU window on this device is ~3.7 ms, so the hazard is real.
//
// The model here is the minimal honest one:
//   * a block is referenced by the open command buffer when the pool hands it out while a
//     SubmitScope is active;
//   * SubmitScope::Commit(signal_value) stamps every referenced block with that value and records
//     the submit in its CompletionDomain;
//   * a scope destroyed without Commit releases its references: the command buffer was never
//     submitted, so nothing can read those blocks;
//   * a block is reusable only when it has no references and its stamped value is complete.
//
// Domains keep values from different queues apart: comparing a side-queue fence with the main
// timeline would be meaningless.
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "common/types.h"

namespace VideoCore { struct StreamBuffer; }

namespace Vulkan {

enum class CompletionDomainId : u32 {
    Main = 0, ///< the graphics queue timeline (Scheduler)
    Side = 1, ///< the side queue (BufferCache downloads)
    Count = 2,
};

struct CompletionToken {
    CompletionDomainId domain{CompletionDomainId::Main};
    u64 value{0};
};

class CompletionRegistry {
public:
    static CompletionRegistry& Instance() {
        static CompletionRegistry registry;
        return registry;
    }

    void Submitted(CompletionDomainId domain, u64 value) {
        auto& slot = slots[static_cast<u32>(domain)];
        u64 seen = slot.submitted.load(std::memory_order_relaxed);
        while (seen < value &&
               !slot.submitted.compare_exchange_weak(seen, value, std::memory_order_release)) {
        }
    }

    void Completed(CompletionDomainId domain, u64 value) {
        auto& slot = slots[static_cast<u32>(domain)];
        u64 seen = slot.completed.load(std::memory_order_relaxed);
        while (seen < value &&
               !slot.completed.compare_exchange_weak(seen, value, std::memory_order_release)) {
        }
    }

    [[nodiscard]] u64 LastCompleted(CompletionDomainId domain) const {
        return slots[static_cast<u32>(domain)].completed.load(std::memory_order_acquire);
    }

    /// True when the value this token waits for has been completed on its own domain. Values are
    /// never compared across domains.
    [[nodiscard]] bool IsComplete(const CompletionToken& token) const {
        return LastCompleted(token.domain) >= token.value;
    }

private:
    struct Slot {
        std::atomic<u64> submitted{0};
        std::atomic<u64> completed{0};
    };
    Slot slots[static_cast<u32>(CompletionDomainId::Count)]{};
};

/// One command buffer's worth of references. Created by the Scheduler before recording, committed
/// at the submit, or dropped (releasing the references) if the command buffer is never submitted.
class SubmitScope {
public:
    explicit SubmitScope(CompletionDomainId domain_) : domain{domain_} {}

    ~SubmitScope() {
        // Destroyed without Commit: the command buffer will never reach the GPU.
        Abort();
    }

    SubmitScope(const SubmitScope&) = delete;
    SubmitScope& operator=(const SubmitScope&) = delete;

    void Reference(VideoCore::StreamBuffer* buffer) {
        if (buffer == nullptr || committed) {
            return;
        }
        buffers.push_back(buffer);
    }

    /// Stamps every referenced block with the value this submit signals.
    void Commit(u64 signal_value);

    /// Releases the references without stamping them.
    void Abort();

    [[nodiscard]] bool IsCommitted() const {
        return committed;
    }

    [[nodiscard]] CompletionDomainId Domain() const {
        return domain;
    }

    [[nodiscard]] size_t ReferenceCount() const {
        return buffers.size();
    }

private:
    CompletionDomainId domain{CompletionDomainId::Main};
    std::vector<VideoCore::StreamBuffer*> buffers;
    bool committed{false};
};

} // namespace Vulkan
