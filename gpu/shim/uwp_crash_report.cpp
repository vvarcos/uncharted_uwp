extern "C" int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped);
// SPDX-License-Identifier: GPL-2.0-or-later
// UWP M5: process-wide crash reporter.
//
// The renderer boot test dies without a log line and without a C++ exception reaching the
// caller, so the cause has to be classified from the exception record itself. Installed from
// loader() before the renderer objects are built, it appends one line per first-chance
// exception to LocalState\crash.log: code, faulting address, owning module and offset, thread,
// the raw parameters (never dereferenced blindly - the shape depends on the exception code),
// the MSVC C++ exception type when the record is 0xE06D7363, and up to 24 stack frames as
// module+offset so the throw site can be symbolized offline with the PDB.
//
// AppContainer notes: GetModuleFileNameW/GetThreadDescription/RtlCaptureStackBackTrace are not
// part of the contract, so modules are identified by comparing GetModuleHandleExW results
// against the handles captured at install time, and CaptureStackBackTrace is attempted once
// behind a re-entry guard (it returns 0 when the contract blocks it).

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

#if defined(UNCHARTED_UWP)

namespace {

constexpr unsigned kMaxModules = 16;
constexpr unsigned kLineCapacity = 1400;
constexpr unsigned kMaxFrames = 24;

wchar_t g_path[MAX_PATH]{};
HMODULE g_modules[kMaxModules]{};
const char* g_module_names[kMaxModules]{};
unsigned g_module_count = 0;
bool g_installed = false;
thread_local bool g_handling = false;
// MS_VC_EXCEPTION (thread naming) is first-chance and harmless; only the count is worth keeping.
std::atomic<unsigned long long> g_thread_name_events{0};

void Append(char* buffer, unsigned& length, const char* text) {
    while (*text && length + 1 < kLineCapacity) {
        buffer[length++] = *text++;
    }
    buffer[length] = '\0';
}

void AppendBounded(char* buffer, unsigned& length, const char* text, unsigned limit) {
    for (unsigned i = 0; i < limit && text[i] != '\0'; ++i) {
        const char c = text[i];
        char single[2]{static_cast<char>(c >= 0x20 && c < 0x7F ? c : '?'), '\0'};
        Append(buffer, length, single);
    }
}

void AppendHex(char* buffer, unsigned& length, unsigned long long value) {
    char digits[16];
    unsigned count = 0;
    if (value == 0) {
        Append(buffer, length, "0");
        return;
    }
    while (value != 0 && count < 16) {
        const unsigned nibble = static_cast<unsigned>(value & 0xF);
        digits[count++] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        value >>= 4;
    }
    while (count > 0) {
        char single[2]{digits[--count], '\0'};
        Append(buffer, length, single);
    }
}

void AppendDec(char* buffer, unsigned& length, unsigned long long value) {
    char digits[20];
    unsigned count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0) {
        char single[2]{digits[--count], '\0'};
        Append(buffer, length, single);
    }
}

void WriteLine(const char* text) {
    if (g_path[0] == L'\0') {
        return;
    }
    const HANDLE file = CreateFileW(g_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
    CloseHandle(file);
}

void RegisterModule(const wchar_t* module_name, const char* short_name) {
    if (g_module_count >= kMaxModules) {
        return;
    }
    const HMODULE handle = GetModuleHandleW(module_name);
    if (handle != nullptr) {
        g_modules[g_module_count] = handle;
        g_module_names[g_module_count] = short_name;
        ++g_module_count;
    }
}

bool Readable(const void* address, unsigned long long size) {
    if (address == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    const auto start = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address));
    const auto end = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info.BaseAddress)) +
                     info.RegionSize;
    return start + size <= end;
}

const char* NameForAddress(const void* address, unsigned long long& offset, HMODULE* owner_out) {
    offset = 0;
    HMODULE owner = nullptr;
    if (address == nullptr ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(address), &owner) ||
        owner == nullptr) {
        return "?";
    }
    if (owner_out != nullptr) {
        *owner_out = owner;
    }
    offset = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address)) -
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(owner));
    for (unsigned i = 0; i < g_module_count; ++i) {
        if (g_modules[i] == owner) {
            return g_module_names[i];
        }
    }
    return "?";
}

// MSVC x64 ABI: _ThrowInfo { u32 attributes; CatchableTypeArray* types; ... }.
// CatchableTypeArray { i32 count; CatchableType* types[]; } with 8-byte alignment.
// CatchableType { u32 properties; TypeDescriptor* descriptor; ... }.
// TypeDescriptor { void* vfTable; void* spare; char name[]; }.
const char* ExceptionTypeName(void* throw_info) {
    if (!Readable(throw_info, 16)) {
        return nullptr;
    }
    const auto* bytes = static_cast<const unsigned char*>(throw_info);
    const void* catchable_array = *reinterpret_cast<const void* const*>(bytes + 8);
    if (!Readable(catchable_array, 16)) {
        return nullptr;
    }
    const int count = *reinterpret_cast<const int*>(catchable_array);
    if (count <= 0) {
        return nullptr;
    }
    const auto* array_bytes = static_cast<const unsigned char*>(catchable_array);
    const void* first = *reinterpret_cast<const void* const*>(array_bytes + 8);
    if (!Readable(first, 16)) {
        return nullptr;
    }
    const void* descriptor = *reinterpret_cast<const void* const*>(
        static_cast<const unsigned char*>(first) + 8);
    if (!Readable(descriptor, 24)) {
        return nullptr;
    }
    const char* name = static_cast<const char*>(descriptor) + 16;
    if (!Readable(name, 1)) {
        return nullptr;
    }
    return name;
}

LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr || g_handling) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    // Only hard failures are recorded. First-chance C++ exceptions (0xE06D7363) are part of
    // normal operation (the game-file broker probe throws and is caught), and running the
    // handler - Win32 calls inside the exception context - for each of them is itself a source
    // of instability in AppContainer. 0x406D1388 is MS_VC_EXCEPTION, raised by thread naming: with
    // 17 guest threads it filled crash.log with 3 KB of noise that could hide a real fault.
    if (record->ExceptionCode == 0xE06D7363) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (record->ExceptionCode == 0x406D1388) {
        const unsigned long long seen = g_thread_name_events.fetch_add(1) + 1;
        if (seen == 1 || seen == 100 || (seen % 1000) == 0) {
            char line[96];
            std::snprintf(line, sizeof(line), "crash filter: %llu thread-name events skipped\n", seen);
            WriteLine(line);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_handling = true;

    char line[kLineCapacity]{};
    unsigned length = 0;
    Append(line, length, "crash code=0x");
    AppendHex(line, length, record->ExceptionCode);
    Append(line, length, " address=0x");
    AppendHex(line, length,
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record->ExceptionAddress)));

    unsigned long long offset = 0;
    HMODULE owner = nullptr;
    const char* module = NameForAddress(record->ExceptionAddress, offset, &owner);
    Append(line, length, " module=");
    Append(line, length, module);
    Append(line, length, "+0x");
    AppendHex(line, length, offset);
    if (module[0] == '?') {
        Append(line, length, " module_base=0x");
        AppendHex(line, length,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(owner)));
    }

    Append(line, length, " thread=");
    AppendDec(line, length, GetCurrentThreadId());
    Append(line, length, " params=");
    AppendDec(line, length, record->NumberParameters);

    const unsigned parameters = record->NumberParameters < 4 ? record->NumberParameters : 4;
    for (unsigned i = 0; i < parameters; ++i) {
        Append(line, length, " p");
        AppendDec(line, length, i);
        Append(line, length, "=0x");
        AppendHex(line, length, static_cast<unsigned long long>(record->ExceptionInformation[i]));
    }

    if (record->ExceptionCode == 0xC0000005 && record->NumberParameters >= 2) {
        const unsigned long long access = record->ExceptionInformation[0];
        Append(line, length, " access=");
        Append(line, length, access == 0 ? "read" : (access == 1 ? "write" : "execute"));
        unsigned long long fault_offset = 0;
        HMODULE fault_owner = nullptr;
        const char* fault_module = NameForAddress(
            reinterpret_cast<const void*>(record->ExceptionInformation[1]), fault_offset,
            &fault_owner);
        Append(line, length, " fault_module=");
        Append(line, length, fault_module);
        Append(line, length, "+0x");
        AppendHex(line, length, fault_offset);
        if (fault_module[0] == '?') {
            Append(line, length, " fault_base=0x");
            AppendHex(line, length,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fault_owner)));
        }
        {
            // bbport: is the faulting guest address inside a reserved region? Outside means the
            // commit-on-touch handler cannot materialise it, so the AV is real and not ours.
            uintptr_t rstart = 0, rend = 0;
            int rmapped = 0;
            const int in_region = runtime_memory_region(
                static_cast<uintptr_t>(record->ExceptionInformation[1]), &rstart, &rend, &rmapped);
            Append(line, length, " guest_region=");
            Append(line, length, in_region ? "in" : "OUT");
            Append(line, length, " mapped=");
            Append(line, length, rmapped ? "1" : "0");
            Append(line, length, " range=0x");
            AppendHex(line, length, static_cast<unsigned long long>(rstart));
            Append(line, length, "-0x");
            AppendHex(line, length, static_cast<unsigned long long>(rend));
        }
    }
    if (record->ExceptionCode == 0xE06D7363 && record->NumberParameters >= 4) {
        // p3 is the image base of the throwing module. p2 is a _ThrowInfo (not a type_info).
        unsigned long long base_offset = 0;
        HMODULE base_owner = nullptr;
        const char* base_module = NameForAddress(
            reinterpret_cast<const void*>(record->ExceptionInformation[3]), base_offset,
            &base_owner);
        Append(line, length, " cxx_module=");
        Append(line, length, base_module);
        Append(line, length, "+0x");
        AppendHex(line, length, base_offset);
        if (const char* type = ExceptionTypeName(
                reinterpret_cast<void*>(record->ExceptionInformation[2]))) {
            Append(line, length, " type=");
            AppendBounded(line, length, type, 96);
        }
    }

    void* frames[kMaxFrames]{};
    const unsigned short frame_count = CaptureStackBackTrace(1, kMaxFrames, frames, nullptr);
    Append(line, length, " frames=");
    AppendDec(line, length, frame_count);
    for (unsigned short i = 0; i < frame_count; ++i) {
        unsigned long long frame_offset = 0;
        HMODULE frame_owner = nullptr;
        const char* frame_module = NameForAddress(frames[i], frame_offset, &frame_owner);
        Append(line, length, " f");
        AppendDec(line, length, i);
        Append(line, length, "=");
        Append(line, length, frame_module);
        Append(line, length, "+0x");
        AppendHex(line, length, frame_offset);
        if (frame_module[0] == '?') {
            Append(line, length, "@0x");
            AppendHex(line, length,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frame_owner)));
        }
    }

    // A fault at address 0 (execute/read) is a call through a null pointer; the return address
    // is still on the stack. Record RIP/RSP and the first stack words so the caller is known.
    if (record->ExceptionCode == 0xC0000005 && info->ContextRecord != nullptr) {
        const CONTEXT* context = info->ContextRecord;
        Append(line, length, " rip=0x");
        AppendHex(line, length, static_cast<unsigned long long>(context->Rip));
        Append(line, length, " rsp=0x");
        AppendHex(line, length, static_cast<unsigned long long>(context->Rsp));
        const auto* stack = reinterpret_cast<const unsigned long long*>(context->Rsp);
        if (Readable(stack, sizeof(unsigned long long) * 6)) {
            for (unsigned i = 0; i < 6; ++i) {
                unsigned long long slot_offset = 0;
                HMODULE slot_owner = nullptr;
                const char* slot_module = NameForAddress(
                    reinterpret_cast<const void*>(stack[i]), slot_offset, &slot_owner);
                Append(line, length, " s");
                AppendDec(line, length, i);
                Append(line, length, "=");
                Append(line, length, slot_module);
                Append(line, length, "+0x");
                AppendHex(line, length, slot_offset);
            }
        }
    }
    Append(line, length, "\n");
    WriteLine(line);
    g_handling = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

extern "C" void bb_install_crash_report(const wchar_t* local_state_path) {
    if (g_installed || local_state_path == nullptr || local_state_path[0] == L'\0') {
        return;
    }
    // "<LocalState>\crash.log" without CRT string helpers.
    unsigned length = 0;
    while (local_state_path[length] != L'\0' && length + 11 < MAX_PATH) {
        g_path[length] = local_state_path[length];
        ++length;
    }
    const wchar_t suffix[] = L"\\crash.log";
    for (unsigned i = 0; suffix[i] != L'\0' && length + 1 < MAX_PATH; ++i) {
        g_path[length++] = suffix[i];
    }
    g_path[length] = L'\0';

    RegisterModule(nullptr, "exe");
    RegisterModule(L"bbgpu_dll.dll", "bbgpu_dll");
    RegisterModule(L"vulkan_dzn.dll", "vulkan_dzn");
    RegisterModule(L"dxil.dll", "dxil");
    RegisterModule(L"vcruntime140_app.dll", "vcruntime140_app");
    RegisterModule(L"vcruntime140_1_app.dll", "vcruntime140_1_app");
    RegisterModule(L"msvcp140_app.dll", "msvcp140_app");
    RegisterModule(L"msvcp140_atomic_wait_app.dll", "msvcp140_atomic_wait_app");
    RegisterModule(L"ucrtbase_app.dll", "ucrtbase_app");
    RegisterModule(L"kernel32.dll", "kernel32");
    RegisterModule(L"kernelbase.dll", "kernelbase");
    RegisterModule(L"ntdll.dll", "ntdll");
    RegisterModule(L"d3d12.dll", "d3d12");
    RegisterModule(L"dxgi.dll", "dxgi");
    RegisterModule(L"combase.dll", "combase");

    if (AddVectoredExceptionHandler(1, CrashHandler) != nullptr) {
        g_installed = true;
    }
}

// Accessors so loader() can print the captured module bases after stdout is redirected; a
// module the table cannot name in a crash line can then be matched offline by base address.
extern "C" unsigned bb_crash_module_count() {
    return g_module_count;
}

extern "C" const char* bb_crash_module_name(unsigned index) {
    return index < g_module_count ? g_module_names[index] : "?";
}

extern "C" void* bb_crash_module_base(unsigned index) {
    return index < g_module_count ? static_cast<void*>(g_modules[index]) : nullptr;
}

#endif // UNCHARTED_UWP
