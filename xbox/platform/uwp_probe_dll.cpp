// UWP probe DLL: proves that a packaged native DLL can be loaded with LoadPackagedLibrary and
// called from the app. The renderer will move into a DLL like this one to stay under the code
// size that the console accepts in the main image.
#include <windows.h>

extern "C" __declspec(dllexport) int bbprobe_answer() {
    return 42;
}

extern "C" __declspec(dllexport) int bbprobe_add(int a, int b) {
    return a + b;
}

// Diagnostic: try to load the renderer DLL from the package and report the loader error.
extern "C" __declspec(dllexport) int bbprobe_load_renderer() {
    HMODULE dll = LoadPackagedLibrary(L"bbgpu_dll.dll", 0);
    if (!dll) {
        return static_cast<int>(GetLastError());
    }
    FreeLibrary(dll);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
    return TRUE;
}
