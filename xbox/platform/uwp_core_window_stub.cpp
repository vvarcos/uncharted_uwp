// The CoreWindow accessor lives in the application view; when the DLL is built without the
// graphics host path it still needs the symbol, so this fallback reports "no window".
struct IUnknown;
extern "C" IUnknown* uncharted_uwp_core_window() {
    return nullptr;
}
