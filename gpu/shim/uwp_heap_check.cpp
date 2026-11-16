// UWP M5: heap validator checkpoints.
//
// The renderer boot dies in different places from run to run (STATUS_HEAP_CORRUPTION 0xC0000374,
// a silent __fastfail, a fault inside the driver) and the write that corrupts the process heap is
// never the one that reports it: the heap validator only notices at the next heap operation. This
// walks the process heap at chosen checkpoints and prints the first one that is already broken, so
// the write is narrowed to the call between two checkpoints.
//
// Off unless UNCHARTED_M5_HEADLESS_DEVICE (the diagnostic boot): HeapValidate is not free.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(UNCHARTED_UWP) && defined(UNCHARTED_M5_HEADLESS_DEVICE)

namespace {
// 0 = never found one, so the checkpoints after the first failure stay quiet.
bool g_broken = false;
} // namespace

extern "C" void bb_heap_check(const char* where) {
    if (g_broken || where == nullptr) {
        return;
    }
    if (HeapValidate(GetProcessHeap(), 0, nullptr)) {
        std::printf("heap ok: %s\n", where);
    } else {
        std::printf("HEAP CORRUPT first seen at: %s\n", where);
        g_broken = true;
    }
    std::fflush(stdout);
}

#else

extern "C" void bb_heap_check(const char*) {}

#endif

// Raw marker channel, independent of stdio: the boot log ends at the first print after the renderer
// teardown and it is not known yet whether that print blocks (stream/pipe) or the thread is frozen
// before it. A CreateFileW/WriteFile line proves the thread got there even if stdio never returns.
namespace {
// Full path: the process working directory is not LocalState, so a relative name never opens.
wchar_t g_raw_path[MAX_PATH]{};
} // namespace

extern "C" void bb_log_raw_init(const wchar_t* local_state) {
    if (local_state == nullptr || local_state[0] == L'\0') {
        return;
    }
    unsigned length = 0;
    while (local_state[length] != L'\0' && length + 10 < MAX_PATH) {
        g_raw_path[length] = local_state[length];
        ++length;
    }
    const wchar_t suffix[] = L"\\raw.log";
    for (unsigned i = 0; suffix[i] != L'\0' && length + 1 < MAX_PATH; ++i) {
        g_raw_path[length++] = suffix[i];
    }
    g_raw_path[length] = L'\0';
}

extern "C" void bb_log_raw(const char* text) {    if (text == nullptr) {
        return;
    }
    // One WriteFile per record, serialized: two threads writing markers at the same time must not
    // interleave inside a record. The CRT stream is not used here on purpose.
    static CRITICAL_SECTION section{};
    static bool ready = false;
    if (!ready) {
        InitializeCriticalSection(&section);
        ready = true;
    }
    EnterCriticalSection(&section);
    // Before the loader thread has WinRT (the LocalState path comes from ApplicationData), the path
    // arrives through BB_RAW_LOG, set by the UI thread — the one that already writes entry.log and
    // is therefore known to work. This is what makes the window before _wfreopen observable.
    if (g_raw_path[0] == L'\0') {
        const char* env = std::getenv("BB_RAW_LOG");
        if (env == nullptr || env[0] == '\0') {
            LeaveCriticalSection(&section);
            return;
        }
        const HANDLE ansi = CreateFileA(env, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (ansi == INVALID_HANDLE_VALUE) {
            LeaveCriticalSection(&section);
            return;
        }
        DWORD written = 0;
        size_t length = 0;
        while (text[length] != '\0') {
            ++length;
        }
        WriteFile(ansi, text, static_cast<DWORD>(length), &written, nullptr);
        WriteFile(ansi, "\n", 1, &written, nullptr);
        CloseHandle(ansi);
        LeaveCriticalSection(&section);
        return;
    }
    const HANDLE file = CreateFileW(g_raw_path, FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&section);
        return;
    }
    DWORD written = 0;
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
    WriteFile(file, "\n", 1, &written, nullptr);
    CloseHandle(file);
    LeaveCriticalSection(&section);
}

/* bbport: independent liveness channel. Own file, own handle, no critical section, no WinRT, no
 * allocation: if this keeps growing while loader.log stands still, the process is alive and the log
 * path (or a subsystem) is what stalled. Each record is a u32 sequence number plus the tick count,
 * so a reader can tell "alive" from "alive but the clock stopped". */
/* bbport: leave it running: the frozen log says nothing about liveness. */
static HANDLE g_heartbeat_file = INVALID_HANDLE_VALUE;

extern "C" void bb_heartbeat_mark(unsigned stage) {
    if (g_heartbeat_file == INVALID_HANDLE_VALUE) {
        return;
    }
    const DWORD record[2] = {0x80000000u | stage, GetTickCount()};
    DWORD written = 0;
    WriteFile(g_heartbeat_file, record, sizeof(record), &written, nullptr);
}

/* Text line in the same file, for values a 32-bit record cannot carry (lock holder and site). */
extern "C" void bb_heartbeat_text(const char* text) {
    if (g_heartbeat_file == INVALID_HANDLE_VALUE || text == nullptr) {
        return;
    }
    DWORD written = 0;
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    WriteFile(g_heartbeat_file, text, static_cast<DWORD>(length), &written, nullptr);
    WriteFile(g_heartbeat_file, "\n", 1, &written, nullptr);
}

extern "C" void bb_heartbeat_start(const wchar_t* local_state) {
    if (local_state == nullptr || local_state[0] == L'\0') {
        return;
    }
    std::wstring path{local_state};
    path += L"\\heartbeat.bin";
    static std::wstring s_path = path;
    g_heartbeat_file = CreateFileW(s_path.c_str(), FILE_APPEND_DATA,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    std::thread([] {
        const HANDLE file = CreateFileW(s_path.c_str(), FILE_APPEND_DATA,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return;
        }
        for (unsigned sequence = 0;; ++sequence) {
            const DWORD record[2] = {sequence, GetTickCount()};
            DWORD written = 0;
            WriteFile(file, record, sizeof(record), &written, nullptr);
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();
}

