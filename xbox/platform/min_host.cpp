// Minimal UWP host used as an activation probe: a full CoreApplication view with no renderer and
// no packaged DLL, so a successful start isolates "the executable is valid" from "the DLL or the
// package contents are wrong". It writes a log next to the app's LocalState and exits immediately.
#include <windows.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <cstdio>

namespace {
std::wstring LocalStatePath() {
    try {
        return winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str();
    } catch (...) {
        return L"";
    }
}

void Log(const char* line);

bool Readable(const void* address, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) == 0) {
        return false;
    }
    const bool committed = info.State == MEM_COMMIT;
    const bool readable = (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                                           PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
    return committed && readable;
}

// Walks the MSVC throw record: _ThrowInfo -> pCatchableTypeArray -> types[0] -> pTypeInfo -> name.
// Every offset in the record is an RVA relative to the image base stored in ExceptionInformation[3].
void LogCppExceptionType(const EXCEPTION_RECORD* record) {
    const auto image_base =
        reinterpret_cast<const unsigned char*>(record->ExceptionInformation[3]);
    const auto throw_info =
        reinterpret_cast<const unsigned char*>(record->ExceptionInformation[2]);
    if (image_base == nullptr || throw_info == nullptr || !Readable(throw_info, 16)) {
        return;
    }
    const auto* info = reinterpret_cast<const unsigned*>(throw_info);
    const unsigned cta_rva = info[3];
    const auto* catchable_array = image_base + cta_rva;
    if (!Readable(catchable_array, 8)) {
        return;
    }
    const auto* array = reinterpret_cast<const int*>(catchable_array);
    if (array[0] < 1 || !Readable(catchable_array + 4, 4)) {
        return;
    }
    // catchable_array[1] is an RVA to _CatchableType, whose second field (offset 4) is the RVA to
    // the TypeDescriptor; the decorated name follows its two pointers.
    const unsigned ct_rva = *reinterpret_cast<const unsigned*>(catchable_array + 4);
    const auto* catchable_type = image_base + ct_rva;
    if (!Readable(catchable_type, 8)) {
        return;
    }
    const unsigned type_rva = *reinterpret_cast<const unsigned*>(catchable_type + 4);
    const auto* type_descriptor = image_base + type_rva;
    if (!Readable(type_descriptor, 24)) {
        return;
    }
    const auto* name = reinterpret_cast<const char*>(type_descriptor + 16);
    if (!Readable(name, 1)) {
        return;
    }
    char line[256]{};
    std::snprintf(line, sizeof(line), "host: veh cpp_type=%.180s", name);
    Log(line);
}
void Log(const char* line) {
    static bool first_write = true;
    const std::wstring local = LocalStatePath();
    if (local.empty()) {
        return;
    }
    // Truncate on the first write of each process run, so old lines can never be mistaken for a
    // fresh result (that ambiguity invalidated one experiment already).
    const wchar_t* mode = first_write ? L"w" : L"a";
    first_write = false;
    if (FILE* file = _wfopen((local + L"\\min_host.log").c_str(), mode)) {
        std::fprintf(file, "%s\n", line);
        std::fclose(file);
    }
}

struct View : winrt::implements<View, winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    void Initialize(const winrt::Windows::ApplicationModel::Core::CoreApplicationView&) {
        Log("min_host: Initialize");
    }
    void SetWindow(const winrt::Windows::UI::Core::CoreWindow&) { Log("min_host: SetWindow"); }
    void Load(winrt::hstring) { Log("min_host: Load"); }
    void Uninitialize() { Log("min_host: Uninitialize"); }
    void Run() {
        Log("min_host: Run");
        winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
    }
};

struct ViewSource
    : winrt::implements<ViewSource,
                        winrt::Windows::ApplicationModel::Core::IFrameworkViewSource> {
    winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView() {
        return winrt::make<View>();
    }
};
} // namespace

LONG CALLBACK VehLogger(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(record->ExceptionAddress), &module);
    char line[320]{};
    std::snprintf(line, sizeof(line), "host: veh code=0x%08lx addr=%p rva=0x%llx nparams=%lu",
                  record->ExceptionCode, record->ExceptionAddress,
                  module != nullptr
                      ? reinterpret_cast<unsigned long long>(record->ExceptionAddress) -
                            reinterpret_cast<unsigned long long>(module)
                      : reinterpret_cast<unsigned long long>(record->ExceptionAddress),
                  record->NumberParameters);
    Log(line);
    {
        void* frames[16]{};
        const USHORT count = CaptureStackBackTrace(0, 16, frames, nullptr);
        for (USHORT i = 0; i < count && i < 8; ++i) {
            HMODULE frame_module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(frames[i]), &frame_module);
            char frame_line[200]{};
            std::snprintf(frame_line, sizeof(frame_line), "host: veh frame[%u] rva=0x%llx", i,
                          frame_module != nullptr
                              ? reinterpret_cast<unsigned long long>(frames[i]) -
                                    reinterpret_cast<unsigned long long>(frame_module)
                              : reinterpret_cast<unsigned long long>(frames[i]));
            Log(frame_line);
        }
    }
    // Raw exception parameters only: dereferencing them without knowing the ABI shape crashed
    // the handler earlier, so this pass records the values and the module each points into.
    if (record->ExceptionCode == 0xE06D7363 && record->NumberParameters >= 4) {
        for (unsigned i = 0; i < 4; ++i) {
            const auto value = static_cast<unsigned long long>(record->ExceptionInformation[i]);
            HMODULE owner = nullptr;
            if (value > 0x10000) {
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(value), &owner);
            }
            // GetModuleFileNameW is not available to AppContainer apps, so match the owner base
            // against the modules this process is known to load.
            const wchar_t* candidates[] = {L"bbgpu_dll.dll", L"vcruntime140_app.dll",
                                           L"vcruntime140_1_app.dll", L"msvcp140_app.dll",
                                           L"msvcp140_atomic_wait_app.dll", L"vulkan_dzn.dll"};
            const char* owner_name = "?";
            for (const wchar_t* candidate : candidates) {
                if (GetModuleHandleW(candidate) == owner && owner != nullptr) {
                    owner_name = candidate == candidates[0]   ? "bbgpu_dll"
                                 : candidate == candidates[1] ? "vcruntime140_app"
                                 : candidate == candidates[2] ? "vcruntime140_1_app"
                                 : candidate == candidates[3] ? "msvcp140_app"
                                 : candidate == candidates[4] ? "msvcp140_atomic_wait_app"
                                                              : "vulkan_dzn";
                    break;
                }
            }
            char part[400]{};
            std::snprintf(part, sizeof(part), "host: veh param[%u]=0x%llx owner=%p module=%s", i,
                          value, reinterpret_cast<void*>(owner), owner_name);
            Log(part);
        }
        LogCppExceptionType(record);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    {
        char stamp[160]{};
        std::snprintf(stamp, sizeof(stamp), "host: build %s %s", __DATE__, __TIME__);
        Log(stamp);
    }
    // The DLL's static initializers touch WinRT/COM; give the thread an apartment before the
    // load so a CO_E_NOTINITIALIZED cannot escape _initterm and become 1114.
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        Log("host: apartment initialized (STA) before load");
    } catch (winrt::hresult_error const& error) {
        char line[160]{};
        std::snprintf(line, sizeof(line), "host: init_apartment hr=0x%08lx",
                      static_cast<unsigned long>(error.code()));
        Log(line);
    } catch (...) {
        Log("host: init_apartment unknown exception");
    }
    PVOID veh = AddVectoredExceptionHandler(1, VehLogger);
    SetLastError(0);
    HMODULE dll = LoadPackagedLibrary(L"bbgpu_dll.dll", 0);
    const DWORD load_error = GetLastError();
    RemoveVectoredExceptionHandler(veh);
    if (!dll) {
        char line[128]{};
        std::snprintf(line, sizeof(line), "host: LoadPackagedLibrary failed error=%lu",
                      GetLastError());
        Log(line);
        return 1;
    }
    Log("host: dll loaded");
    using Entry = int (*)(void*, void*);
    auto entry = reinterpret_cast<Entry>(GetProcAddress(dll, "bbgpu_uwp_app_main"));
    if (!entry) {
        char line[128]{};
        std::snprintf(line, sizeof(line), "host: entry missing error=%lu", GetLastError());
        Log(line);
        return 2;
    }
    Log("host: calling app entry");
    // The DLL owns the Vulkan instance: creating it here linked the whole renderer into the
    // executable as well, duplicating every global across two modules.
    const int result = entry(nullptr, nullptr);
    char line[128]{};
    std::snprintf(line, sizeof(line), "host: app entry returned %d", result);
    Log(line);
    return result;
}
