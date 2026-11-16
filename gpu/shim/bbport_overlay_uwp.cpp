// SPDX-License-Identifier: GPL-2.0-or-later
// UWP: the in-game menu is desktop-only (SDL window, SDL events, ImGui backend). This inert
// implementation keeps the renderer linkable on the console; it draws nothing and consumes no
// input. Replacing it with a CoreWindow/Gamepad menu is future work, not part of this gate.
#ifdef UNCHARTED_UWP
#include <cstdio>
#include "bbport_overlay.h"

namespace BbOverlay {

void Init(const Vulkan::Instance&, vk::Format, u32) {}

bool HandleEvent(const SDL_Event&) {
    return false;
}

void UpdateTextInput(SDL_Window*) {}

bool Visible() {
    return false;
}

void Render(vk::CommandBuffer, vk::ImageView, vk::Extent2D) {}

void NotePresent() {}

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
