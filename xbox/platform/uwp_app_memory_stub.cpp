// UWP renderer DLL: the app-memory reporting helpers live in the executable (they use WinRT).
// The diagnostic DLL links its own copies so the runtime library resolves; they report zero and
// say so, instead of pretending to measure the app.
#include <cstdint>
#include <cstdio>

extern "C" uint64_t uncharted_uwp_app_memory_usage() {
    static bool warned = false;
    if (!warned) {
        warned = true;
        std::puts("M5 dll: app memory reporting is a stub inside the renderer DLL");
    }
    return 0;
}

extern "C" uint64_t uncharted_uwp_app_memory_limit() {
    return 0;
}

extern "C" void uncharted_uwp_pager_thread_init() {}



