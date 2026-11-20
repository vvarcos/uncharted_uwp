// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: per-frame counters for the 3-4 FPS investigation. The renderer increments them at the
// three suspected costs (arena replacement sync, arena rebuilds, QUAD_LIST expansion); the UWP HUD
// snapshots the totals on every present and prints the delta, so a console screenshot carries the
// numbers even when no log is pulled.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace BbPort::Counters {

struct Totals {
    std::atomic<uint64_t> scheduler_finishes{0};
    std::atomic<uint64_t> scheduler_finish_ns{0};
    std::atomic<uint64_t> arena_replacements{0};
    std::atomic<uint64_t> quad_draws{0};
    std::atomic<uint64_t> quad_indices{0};
};

inline Totals& Get() {
    static Totals totals;
    return totals;
}

/// Increment helpers: one call site each, so the counters cannot drift from the code they measure.
inline void NoteSchedulerFinish(std::chrono::steady_clock::duration elapsed) {
    auto& totals = Get();
    totals.scheduler_finishes.fetch_add(1, std::memory_order_relaxed);
    totals.scheduler_finish_ns.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        std::memory_order_relaxed);
}

inline void NoteArenaReplacement() {
    Get().arena_replacements.fetch_add(1, std::memory_order_relaxed);
}

inline void NoteQuadExpansion(uint64_t expanded_indices) {
    auto& totals = Get();
    totals.quad_draws.fetch_add(1, std::memory_order_relaxed);
    totals.quad_indices.fetch_add(expanded_indices, std::memory_order_relaxed);
}

/// Delta since the previous call. Called once per present, from the presentation thread only:
/// the `last_*` baselines below are plain integers, so a second present thread would race.
struct Frame {
    uint64_t scheduler_finishes{};
    uint64_t scheduler_finish_us{};
    uint64_t arena_replacements{};
    uint64_t quad_draws{};
    uint64_t quad_indices{};
};

inline Frame TakeFrame() {
    static uint64_t last_finishes{};
    static uint64_t last_finish_ns{};
    static uint64_t last_arena{};
    static uint64_t last_quads{};
    static uint64_t last_indices{};
    auto& totals = Get();
    const uint64_t finishes = totals.scheduler_finishes.load(std::memory_order_relaxed);
    const uint64_t finish_ns = totals.scheduler_finish_ns.load(std::memory_order_relaxed);
    const uint64_t arena = totals.arena_replacements.load(std::memory_order_relaxed);
    const uint64_t quads = totals.quad_draws.load(std::memory_order_relaxed);
    const uint64_t indices = totals.quad_indices.load(std::memory_order_relaxed);
    Frame frame;
    frame.scheduler_finishes = finishes - last_finishes;
    frame.scheduler_finish_us = (finish_ns - last_finish_ns) / 1000;
    frame.arena_replacements = arena - last_arena;
    frame.quad_draws = quads - last_quads;
    frame.quad_indices = indices - last_indices;
    last_finishes = finishes;
    last_finish_ns = finish_ns;
    last_arena = arena;
    last_quads = quads;
    last_indices = indices;
    return frame;
}

} // namespace BbPort::Counters
