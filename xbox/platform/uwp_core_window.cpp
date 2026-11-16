// CoreWindow accessor shared by the executable and the renderer DLL. The window itself is owned
// by the application view; this file only stores a borrowed reference and hands it out with an
// extra reference for the caller.
#include <windows.h>
#include <unknwn.h>

namespace {
IUnknown* g_core_window = nullptr;
}

extern "C" void uncharted_uwp_set_core_window(IUnknown* window) {
    if (g_core_window != nullptr) {
        g_core_window->Release();
    }
    g_core_window = window;
    if (g_core_window != nullptr) {
        g_core_window->AddRef();
    }
}

extern "C" IUnknown* uncharted_uwp_core_window() {
    if (g_core_window == nullptr) {
        return nullptr;
    }
    g_core_window->AddRef();
    return g_core_window;
}
