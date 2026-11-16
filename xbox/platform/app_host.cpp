// Minimal UWP host: loads the packaged application DLL and calls its entry point. Import-time
// loading of packaged DLLs is rejected by the console, so this stays a runtime load.
#include <windows.h>
#include <cstdio>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    HMODULE dll = LoadPackagedLibrary(L"bbgpu_dll.dll", 0);
    if (!dll) {
        unsigned long error = GetLastError();
        wchar_t buffer[256];
        swprintf(buffer, 256, L"load bbgpu_dll.dll failed: %lu\n", error);
        OutputDebugStringW(buffer);
        return static_cast<int>(error);
    }
    auto entry = reinterpret_cast<int (*)()>(GetProcAddress(dll, "bbgpu_uwp_app_main"));
    if (!entry) {
        OutputDebugStringW(L"bbgpu_uwp_app_main missing\n");
        return 2;
    }
    return entry();
}
