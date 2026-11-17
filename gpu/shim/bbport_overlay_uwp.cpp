// SPDX-License-Identifier: GPL-2.0-or-later
// UWP: the in-game menu is desktop-only (SDL window, SDL events, ImGui backend). This inert
// implementation keeps the renderer linkable on the console; it draws nothing and consumes no
// input. Replacing it with a CoreWindow/Gamepad menu is future work, not part of this gate.
#ifdef UNCHARTED_UWP
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <winrt/Windows.System.h>
#include "bbport_overlay.h"

namespace BbOverlay {
namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point last_present{};
Clock::time_point last_report{};
double frame_ms_avg{};
u64 memory_peak_mib{};
u64 present_count{};
bool hud_enabled{true};
} // namespace

void Init(const Vulkan::Instance&, vk::Format, u32) {
    if (const char* env = std::getenv("BB_HUD"); env && env[0] == '0') {
        hud_enabled = false;
    }
    last_report = Clock::now();
    std::puts("HUD: UWP RAM/FPS telemetry initialized (BB_HUD=0 disables)");
    std::fflush(stdout);
}

bool HandleEvent(const SDL_Event&) {
    return false;
}

void UpdateTextInput(SDL_Window*) {}

bool Visible() {
    return false;
}

void Render(vk::CommandBuffer, vk::ImageView, vk::Extent2D) {}

void NotePresent() {
    if (!hud_enabled) {
        return;
    }
    const auto now = Clock::now();
    if (last_present != Clock::time_point{}) {
        const double ms = std::chrono::duration<double, std::milli>(now - last_present).count();
        frame_ms_avg = frame_ms_avg == 0.0 ? ms : frame_ms_avg * 0.95 + ms * 0.05;
    }
    last_present = now;
    ++present_count;
    // Querying MemoryManager and formatting a line every frame is unnecessary overhead. Sample at
    // most twice per second; this is also the cadence used by the eventual visual glyph upload.
    if (now - last_report < std::chrono::milliseconds(500)) {
        return;
    }
    last_report = now;
    try {
        const u64 current = winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20;
        const u64 limit = winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20;
        memory_peak_mib = std::max(memory_peak_mib, current);
        const double fps = frame_ms_avg > 0.0 ? 1000.0 / frame_ms_avg : 0.0;
        std::printf("HUD: %.1f FPS %.2f ms RAM %llu/%llu/%llu MiB frames=%llu\n", fps,
                    frame_ms_avg, static_cast<unsigned long long>(current),
                    static_cast<unsigned long long>(memory_peak_mib),
                    static_cast<unsigned long long>(limit),
                    static_cast<unsigned long long>(present_count));
        std::fflush(stdout);
    } catch (...) {
        // Telemetry must never take the renderer down.
    }
}

void NotifyTrophy(int grade, std::string game, std::string name, std::vector<unsigned char>) {
    std::printf("BbOverlay: trophy notice (grade %d, %s / %s) not drawn on UWP\n", grade,
                game.c_str(), name.c_str());
}

bool CapturesInput() {
    return false;
}

void SetTextEntry(bool, const std::string&, const std::string&) {}

} // namespace BbOverlay
#endif
