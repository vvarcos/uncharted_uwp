// SPDX-License-Identifier: GPL-2.0-or-later
// UWP: there is no SDL window. The CoreWindow is owned by the host; this stub satisfies the
// WindowSDL interface that the Vulkan swapchain and presenter use for size and event polling.
#ifdef UNCHARTED_UWP
#include "sdl_window.h"

namespace Frontend {

WindowSDL::WindowSDL(s32 width_, s32 height_, const char* title)
    : width{width_}, height{height_} {
    base_title = title ? title : "";
}

WindowSDL::~WindowSDL() = default;

bool WindowSDL::PollEvents() {
    return is_open.load(std::memory_order_relaxed);
}

void WindowSDL::BeginTextInput(const std::string&, const std::string&) {}

int WindowSDL::PollTextInput(std::string&) {
    return 2;
}

void WindowSDL::UpdateTextTitle() {}

} // namespace Frontend
#endif
