#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>
#include <windows.h>
#include <unknwn.h>
#include <gamingdeviceinformation.h>
#include <fileapifromapp.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

extern "C" int uncharted_probe_main(int argc, char** argv);
extern "C" FILE* uncharted_uwp_fopen(const char* path, const char* mode);
extern "C" void runtime_memory_start_paging();
// Crash reporter (gpu/shim/uwp_crash_report.cpp): records hard faults that never reach a catch.
extern "C" void bb_install_crash_report(const wchar_t* local_state_path);
extern "C" unsigned bb_crash_module_count();
extern "C" const char* bb_crash_module_name(unsigned index);
extern "C" void* bb_crash_module_base(unsigned index);
extern "C" uint64_t uncharted_uwp_app_memory_usage() {
    return winrt::Windows::System::MemoryManager::AppMemoryUsage();
}
extern "C" uint64_t uncharted_uwp_app_memory_limit() {
    return winrt::Windows::System::MemoryManager::AppMemoryUsageLimit();
}
extern "C" void uncharted_uwp_pager_thread_init() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}
#ifdef UNCHARTED_BUILD_GRAPHICS
#include "present_bridge.h"
#ifdef UNCHARTED_LINK_RENDERER
#include "video_core/renderer_vulkan/vk_instance.h"
#endif
#endif
#ifdef UNCHARTED_LINK_RENDERER
extern "C" int uncharted_bbgpu_link_smoke();
#endif
#ifdef UNCHARTED_BUILD_GUEST
extern "C" int bbgpu_uwp_kernel_selftest(void);
#endif
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
extern "C" int bbgpu_uwp_videoout_contract_selftest(void);
extern "C" int bbgpu_uwp_gnm_event_selftest(void);
#endif
#ifdef UNCHARTED_M5_HEADLESS_INSTANCE
extern "C" int bbgpu_uwp_headless_instance_smoke(void);
#endif
#ifdef UNCHARTED_M5_HEADLESS_DEVICE
extern "C" int bbgpu_uwp_headless_device_smoke(void);
extern "C" int bbgpu_uwp_renderer_class_device_smoke(void);
extern "C" int bbgpu_uwp_renderer_offscreen_smoke(void);
extern "C" int bbgpu_uwp_renderer_descriptor_smoke(void);
extern "C" int bbgpu_uwp_renderer_compute_smoke(void);
extern "C" int bbgpu_uwp_renderer_hostpass_smoke(void);
extern "C" int bbgpu_uwp_renderer_swapchain_smoke(void);
extern "C" int bbgpu_uwp_renderer_boot_smoke(void);
extern "C" void bb_log_raw(const char* text);
extern "C" void bb_log_raw_init(const wchar_t* local_state);
extern "C" void bb_heartbeat_start(const wchar_t* local_state);
extern "C" int bbgpu_uwp_renderer_probe_smoke(void);
extern "C" int bbgpu_uwp_renderer_sem_smoke(void);
#ifdef UNCHARTED_PAD_BYTES
    // Size-limit experiment: defined in gpu/shim/uwp_pad_blob.cpp.
    extern "C" int bb_pad_touch();
#endif
#ifdef UNCHARTED_TLS_PAD_BYTES
    // TLS-limit experiment: same file.
    extern "C" int bb_tls_touch();
#endif
#endif

static std::atomic<bool> finished{false};
static winrt::agile_ref<winrt::Windows::UI::Core::CoreWindow> core_window;
static std::atomic<bool> window_closed{false};
extern "C" void uncharted_uwp_set_core_window(IUnknown* window);
extern "C" void* uncharted_uwp_memory_manager();

static bool readable(const std::string& path) {
    FILE* file = uncharted_uwp_fopen(path.c_str(), "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

static bool is_dump(const std::string& folder) {
    if (!readable(folder + "\\eboot.bin")) return false;
    return readable(folder + "\\big2-ps4_Shipping.elf") ||
           readable(folder + "\\big3-ps4_Shipping.elf") ||
           readable(folder + "\\sce_sys\\param.sfo");
}

static std::string find_dump() {
    const char* names[] = {"Uncharted", "NathanDrakeCollection", "UnchartedNDC", "CUSA02343", "CUSA02320"};
    for (char drive : {'E', 'F', 'G', 'H'}) {
        for (const char* name : names) {
            std::string folder = std::string(1, drive) + ":\\dumps\\" + name;
            if (is_dump(folder)) return folder;
        }
    }
    return {};
}

static bool copy_stage_file(const std::string& source, const std::string& destination) {
    FILE* input = uncharted_uwp_fopen(source.c_str(), "rb");
    FILE* output = uncharted_uwp_fopen(destination.c_str(), "wb");
    if (!input || !output) {
        if (input) std::fclose(input);
        if (output) std::fclose(output);
        std::printf("M3 stage FAILED: %s -> %s (errno=%d)\n", source.c_str(), destination.c_str(), errno);
        return false;
    }
    constexpr size_t chunk = 1024 * 1024;
    auto buffer = std::make_unique<unsigned char[]>(chunk);
    unsigned long long total = 0;
    bool ok = true;
    for (;;) {
        const size_t count = std::fread(buffer.get(), 1, chunk, input);
        if (count) {
            if (std::fwrite(buffer.get(), 1, count, output) != count) { ok = false; break; }
            total += count;
        }
        if (count != chunk) { if (std::ferror(input)) ok = false; break; }
    }
    if (std::fclose(input) || std::fclose(output)) ok = false;
    std::printf("M3 stage %s: %llu bytes (%s)\n", source.c_str(), total, ok ? "PASS" : "FAIL");
    return ok;
}

static int stage_dump(const std::string& dump, const std::wstring& local) {
    const std::wstring root = local + L"\\Stage";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW((root + L"\\sce_module").c_str(), nullptr);
    CreateDirectoryW((root + L"\\sce_sys").c_str(), nullptr);
    CreateDirectoryW((root + L"\\dvdroot_ps4").c_str(), nullptr);
    CreateDirectoryW((local + L"\\Prepared").c_str(), nullptr);
    CreateDirectoryW((local + L"\\Prepared\\trophies").c_str(), nullptr);
    const std::string out = winrt::to_string(root);
    struct Item { const char* relative; };
    const Item items[] = {{"eboot.bin"}, {"sce_module\\libc.prx"},
                          {"sce_module\\libSceFios2.prx"}, {"sce_sys\\param.sfo"}};
    int failures = 0;
    for (const auto& item : items)
        failures += !copy_stage_file(dump + "\\" + item.relative, out + "\\" + item.relative);
    std::printf("M3 stage complete: %zu files, %d failures\n", std::size(items), failures);
    return failures ? 1 : 0;
}

static void inspect_dump(const std::string& dump) {
    std::printf("Uncharted dump: %s\n", dump.empty() ? "not found" : dump.c_str());
    if (dump.empty()) return;
    const char* files[] = {"eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf",
                           "sce_sys\\param.sfo", "u1data", "u2data", "u3data"};
    for (const char* name : files) {
        const std::string path = dump + "\\" + name;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        DWORD attributes = GetFileAttributesExFromAppW(winrt::to_hstring(path).c_str(),
                                                        GetFileExInfoStandard, &data)
                               ? data.dwFileAttributes
                               : INVALID_FILE_ATTRIBUTES;
        const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
        std::printf("Dump item %-28s %s%s\n", name, exists ? "found" : "missing",
                    exists && (attributes & FILE_ATTRIBUTE_DIRECTORY) ? " (directory)" : "");
    }
    try {
        auto folder = winrt::Windows::Storage::StorageFolder::GetFolderFromPathAsync(
            winrt::to_hstring(dump)).get();
        auto file = folder.GetFileAsync(L"eboot.bin").get();
        std::printf("Game file broker lookup: PASS (%s)\n", winrt::to_string(file.Name()).c_str());
    } catch (const winrt::hresult_error& error) {
        std::printf("Game file broker lookup: 0x%08x (direct Win32 access remains authoritative)\n",
                    static_cast<unsigned>(error.code()));
    }
    std::printf("External dump eboot.bin readable: %s\n",
                readable(dump + "\\eboot.bin") ? "yes" : "no");
}

extern "C" void bbgpu_uwp_create_window(u32 width, u32 height);

static void loader() {
    // Raw markers for the window before stdout is redirected: loader.log can end up empty even
    // though _wfreopen ran, and the crash reporter is only installed after ApplicationData is
    // resolved. BB_RAW_LOG is set by the UI thread, which is known to work.
    bb_log_raw("L0 loader-enter");
    bb_log_raw("L1 before-init-apartment");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    bb_log_raw("L2 after-init-apartment");
#ifdef UNCHARTED_PAD_BYTES
#ifdef UNCHARTED_TLS_PAD_BYTES
    std::printf("UWP TLS probe: pad=%d KiB first-byte=%d\n", UNCHARTED_TLS_PAD_BYTES >> 10, bb_tls_touch());
#endif
    // Size-limit experiment: keeps the inert blob in the image (see uwp_pad_blob.cpp).
    std::printf("UWP image size probe: pad=%d MiB first-byte=%d\n", UNCHARTED_PAD_BYTES >> 20,
                bb_pad_touch());
#endif
    bb_log_raw("L3 before-application-data");
    const auto local = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
    bb_log_raw("L4 after-application-data");
    bb_install_crash_report(local.c_str());
    bb_log_raw_init(local.c_str());
    /* bbport: an independent liveness channel. Both 5 s timers (memory sample and the GNM stats)
     * stopped at the same instant while the process may well have been alive: the only channel that
     * could tell them apart is one that shares nothing with them — its own file, its own handle, no
     * critical section, no WinRT, no allocation. A growing file means the process runs and the log
     * path is what stalled; a frozen one means the whole process is wedged. */
    bb_heartbeat_start(local.c_str());
    _wfreopen((std::wstring(local) + L"\\loader.log").c_str(), L"w", stdout);
    _wfreopen((std::wstring(local) + L"\\errors.log").c_str(), L"w", stderr);
    bb_log_raw("L5 after-freopen");
    // Unbuffered, not line buffered: measured on this UCRT, _IOLBF does NOT flush on newline — the
    // output stays in the 4 KiB buffer until it fills, and since the app never exits the tail was
    // never written (a whole run left loader.log at 38 bytes even though the loader had run to the
    // module loop; the same effect produced the "missing last lines" in every earlier log).
    // The cost is that an unbuffered printf writes one format item at a time, so concurrent
    // threads can interleave inside a line; the atomic markers use bb_log_raw (single WriteFile).
    setvbuf(stdout, nullptr, _IONBF, 0);
    // SetWindow has already published the CoreWindow before this worker starts. The lightweight
    // frontend gives GnmDriver a window object so it creates the real Presenter instead of staying
    // on the headless flip-completion path.
    bbgpu_uwp_create_window(1920, 1080);
    setvbuf(stderr, nullptr, _IONBF, 0);
    bb_log_raw("L6 after-setvbuf");
    std::puts("Crash reporter: LocalState\\crash.log");
    std::fflush(stdout);
    bb_log_raw("L7 after-first-line");
    bb_log_raw("M0 before-module-count");
    const unsigned module_count = bb_crash_module_count();
    bb_log_raw("M1 after-module-count");
    for (unsigned i = 0; i < module_count; ++i) {
        char module_marker[32];
        std::snprintf(module_marker, sizeof(module_marker), "M2 module %u", i);
        bb_log_raw(module_marker);
        std::printf("Crash module: %s base=%p\n", bb_crash_module_name(i),
                    bb_crash_module_base(i));
    }
    bb_log_raw("M3 after-module-loop");
    std::printf("Loader thread: %lu\n", GetCurrentThreadId());

#ifdef UNCHARTED_M5_VIDEO_CONTRACT
    std::printf("Build %s %s\n", __DATE__, __TIME__);
    std::puts("UNCHARTED UWP M5 diagnostic: logical VideoOut only; guest renderer not attached");
#elif defined(UNCHARTED_BUILD_GUEST)
    std::puts("UNCHARTED UWP loader milestone 4");
#elif defined(UNCHARTED_BUILD_GRAPHICS)
    std::puts("UNCHARTED UWP loader milestone 2");
#else
    std::puts("UNCHARTED UWP loader milestone 1");
#endif
    GAMING_DEVICE_MODEL_INFORMATION model{};
    GetGamingDeviceModelInformation(&model);
    std::printf("Device model: 0x%08x; app budget: %llu MiB\n", model.deviceId,
                static_cast<unsigned long long>(winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20));

    void* code = VirtualAllocFromApp(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old{};
    int smoke = -1;
    if (code) {
        const unsigned char instructions[] = {0xb8, 0x2a, 0, 0, 0, 0xc3};
        std::memcpy(code, instructions, sizeof(instructions));
        if (VirtualProtectFromApp(code, 4096, PAGE_EXECUTE_READ, &old)) {
            FlushInstructionCache(GetCurrentProcess(), code, 4096);
            using NativeFunction = int(__attribute__((sysv_abi))*)();
            smoke = reinterpret_cast<NativeFunction>(code)();
        }
        VirtualFree(code, 0, MEM_RELEASE);
    }
    std::printf("Executable System V ABI smoke: %s (%d)\n", smoke == 42 ? "PASS" : "FAIL", smoke);

    std::string mode = "probe", dump;
    std::ifstream run(std::wstring(local) + L"\\run.txt");
    if (run) {
        std::getline(run, mode);
        std::getline(run, dump);
        if (!mode.empty() && mode.back() == '\r') mode.pop_back();
        if (!dump.empty() && dump.back() == '\r') dump.pop_back();
        // Extra "KEY=VALUE" lines set environment variables for this run, so console tests can
        // force hooks such as BB_PAD_PROMPTS_FORCE=2 without rebuilding or a debugger.
        std::string extra;
        while (std::getline(run, extra)) {
            if (!extra.empty() && extra.back() == '\r') extra.pop_back();
            const size_t equals = extra.find('=');
            if (equals != std::string::npos && equals > 0)
                _putenv_s(extra.substr(0, equals).c_str(), extra.substr(equals + 1).c_str());
        }
    }
    if (dump.empty()) dump = find_dump();
    // B1 closing test: the same boot, then walk the allocator up to the first refusal with the
    // non-fatal OOM path on. Kept as a mode so no extra file format is needed.
    if (mode == "renderer-boot-oom") {
        _putenv_s("BB_OOM_INJECT", "ramp");
        _putenv_s("BB_OOM_NONFATAL", "1");
        mode = "renderer-boot";
    }
    // B2: the same boot, then N staging churn cycles through the real pool (prewarm 2).
    if (mode == "renderer-boot-churn") {
        _putenv_s("BB_STAGING_CHURN", "64");
        mode = "renderer-boot";
    }
    // B2-lag (medição): same boot, plus the queue-submit asynchrony characterization.
    if (mode == "renderer-boot-fence") {
        _putenv_s("BB_STAGING_CHURN", "64");
        _putenv_s("BB_FENCE_CHAR", "1");
        mode = "renderer-boot";
    }
    if (mode == "entry-draw") {
        // Risk gate: create the renderer inside the entry path and let the memory sample show the cost.
        _putenv_s("BB_DRAW", "1");
        mode = "entry";
    }
    if (mode == "entry-realgnm") {
        // Step 1+2: register the real GnmDriver into the loader's own table (one resolver) and keep
        // the raw-channel markers around every stage of it. The known-good stub path stays
        // reachable by leaving the variable unset, so the two builds are comparable.
        _putenv_s("BB_REAL_GNM", "1");
        mode = "entry";
    }
    if (mode == "entry-realgnm-draw") {
        // Step 3+4: the real driver submits the guest's DCBs to the GPU command processor thread,
        // and the rasterizer is bound to the same Liverpool so the fences the guest waits on get
        // written. Costs the renderer's memory (measured separately by the samples).
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        mode = "entry";
    }
    if (mode == "entry-realgnm-draw-noprep") {
        // Falsification of the claim inversion: the same run with the draw preparation workers off,
        // so only the GPU command thread ever compiles a program. If the claim wait disappears, the
        // problem is the concurrency model, not the cache.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        mode = "entry";
    }
    if (mode == "entry-staticpso") {
        // One switch: every optional device feature is treated as absent, selecting the conservative
        // pipeline state. If the PSO is created this way, the guilty part is a feature the ICD
        // announces but cannot honour; if it is not, the problem is elsewhere (shader or formats).
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_FORCE_STATIC_PSO", "1");
        mode = "entry";
    }
    if (mode == "entry-mem-pool") {
        // Same as entry-mem with the dedicated staging pool: the ICD charges 1 GiB per host-visible
        // allocation, so keeping the staging blocks in one device memory is the fix under test.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_VRAM_LOG", "1");
        _putenv_s("BB_STAGING_POOL", "1");
        mode = "entry";
    }
    if (mode == "entry-flipclock") {
        // Clock restricted to the FLIP QUEUE (ident 6, no payload): tests whether the guest's own EOP
        // handler is what signals the job sync queue, without inventing a job-to-ident map.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_PRESENT_CLOCK", "1");
        mode = "entry";
    }
    if (mode == "entry-videoout") {
        // Same as entry-unresolved plus the real VideoOut driver (created without the real library, which
        // crashes here). Tests whether the flip completes and signals the guest FLIP QUEUE.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_UNRESOLVED", "log-fail");
        _putenv_s("BB_VIDEOOUT_DRIVER", "1");
        mode = "entry";
    }
    if (mode == "entry-workers") {
        // Same as entry-unresolved but WITHOUT BB_PREP_WORKERS=0: the deferred EOP delivery depends on
        // the recording/copy workers, which that setting disables.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_UNRESOLVED", "log-fail");
        _putenv_s("BB_FLIP_LOG", "1");
        mode = "entry";
    }
    if (mode == "entry-eopwait") {
        // EOP waits until the draw pipe reaches the position of that packet (ordered retire) instead
        // of draining the whole pipe, which faults the guest.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        /* bbport: deliberately NOT setting BB_PREP_WORKERS=0 here - the pipe consumer needs its threads. */
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_UNRESOLVED", "log-fail");
        _putenv_s("BB_EOP_WAITPOS", "1");
        _putenv_s("BB_FLIP_LOG", "1");
        mode = "entry";
    }
    if (mode == "entry-eopinline") {
        // Same as entry-unresolved plus BB_EOP_INLINE=1: the end-of-pipe interrupt is raised inline
        // instead of behind the host copies, which measured 16 of 43 deliveries.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_UNRESOLVED", "log-fail");
        _putenv_s("BB_EOP_INLINE", "1");
        _putenv_s("BB_FLIP_LOG", "1");
        mode = "entry";
    }
    if (mode == "entry-dump-cs") {
        // The seven compute programs the GPU crash history blamed, and nothing else: enough to read
        // the SPIR-V/IR offline without the dump itself shifting the timing of the hang. Falls into
        // entry-unresolved, so the rest of the run is exactly the one that reaches the loss.
        _putenv_s("BB_DUMP_SHADERS",
                  "0x6a6422a9,0xaec539f0,0x7095d6c2,0x4f81a8f,0x1e8e11d0,0x6d71bb,0x19367542");
        // A pipeline loaded from the serialized cache never runs CompileModule, so DumpShader is not
        // even reached; with the cache off every program is compiled again and the dump happens.
        _putenv_s("BB_PIPELINE_CACHE", "0");
        // One line per program this build compiles, with its hash: the dump filter is useless if
        // none of the blamed programs is ever compiled again.
        _putenv_s("BB_COMPILE_LOG", "1");
        mode = "entry-unresolved";
    }
    if (mode == "entry-skip-loop-cs") {
        // Bisect by hash: drop only the last dispatch seen before the loss (19367542, the one compute
        // program with an OpLoopMerge). If the device still hangs, that shader is not the culprit.
        _putenv_s("BB_SKIP_COMPUTE", "0x19367542");
        mode = "entry-unresolved";
    }
    if (mode == "entry-skip-6d71") {
        // Same submit as the last dispatch (tick 13460 in the run of .26320.1912): the other 80x45
        // compute, the only one still able to explain the loss now that 19367542 was dropped.
        _putenv_s("BB_SKIP_COMPUTE", "0x6d71bb");
        mode = "entry-unresolved";
    }
    if (mode == "entry-precompile") {
        // Disposable launcher action: initialize the renderer, warm and persist the existing serialized
        // cache, then `_Exit`. Starting the game afterward gets a fresh process and all RAM back.
        _putenv_s("BB_PRECOMPILE_ONLY", "1");
        _putenv_s("BB_PIPELINE_CACHE", "1");
        _putenv_s("BB_WARM_MODE", "1");
        _putenv_s("BB_WARM_FORCE", "1");
        _putenv_s("BB_REQUIRE_SEED", "1");
        _putenv_s("BB_WARM_THREADS", "1"); // lowest transient RAM; speed is secondary in launcher
        _putenv_s("BB_COMPILE_LOG", "1");
        mode = "entry-unresolved";
    }
    if (mode == "entry-memlog") {
        // Same run as entry-unresolved plus the periodic memory split, so the growing category can be
        // named instead of guessed while the app walks into the 5120 MiB ceiling.
        _putenv_s("BB_VRAM_LOG", "1");
        // One line per program actually compiled: zero `Compile:` lines means the pipeline cache was
        // already warm, so precompiling cannot buy anything on that run.
        _putenv_s("BB_COMPILE_LOG", "1");
        mode = "entry-unresolved";
    }
    if (mode == "entry-decommit") {
        // The guest commit is carried even after a page is unmapped (committed=1878 released=0), which is
        // what walks the app into the 5120 MiB ceiling. This mode releases it: A/B only, since freeing a
        // commit under a page the guest still touches is a use-after-free risk.
        _putenv_s("BB_GUEST_DECOMMIT", "1");
        mode = "entry-unresolved";
    }
    if (mode == "entry-skip-copy-same") {
        // Complement of entry-skip-copy-cross: only the same-format copies are dropped (the CopyImage
        // path). This is the cell that decides which of the two copies sits under the hang.
        _putenv_s("BB_SKIP_COPY", "3");
        mode = "entry-unresolved";
    }
    if (mode == "entry-skip-copy-cross") {
        // Same A/B, but only the depth<->color copies are dropped: the ones that go through the
        // intermediate buffer. Separates that path from the plain color->color copy.
        _putenv_s("BB_SKIP_COPY", "2");
        mode = "entry-unresolved";
    }
    if (mode == "entry-skip-copy") {
        // A/B of the copies: they are now proven to sit in the same submit as the loss, so drop them and
        // see whether the adapter survives. Proof line: "M5 skip copy: ... dropped".
        _putenv_s("BB_SKIP_COPY", "1");
        mode = "entry-unresolved";
    }
    if (mode == "entry-unresolved") {
        // BB_UNRESOLVED=log-fail: an import this build does not have answers ENOSYS instead of ending
        // the process, so one run lists the whole chain the guest asks for.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_UNRESOLVED", "log-fail");
        _putenv_s("BB_VIDEOOUT_DRIVER", "1"); // validated headless real-port bridge
        _putenv_s("BB_GAME_RES", "1280x720"); // Xbox baseline; CoreWindow scales to the swapchain
        /* Scripted input for automated runs: runtime_pad.c reads this file (tokens such as "cross")
         * and ignores the live controller while it is set. Creating the file is what presses a
         * button; emptying it releases. */
        {
            const std::string pad_path = winrt::to_string(local) + "\\pad.txt";
            _putenv_s("BB_PAD_FILE", pad_path.c_str());
        }
        _putenv_s("BB_FLIP_LOG", "1"); // flip-interval histogram every 5 s
        _putenv_s("BB_GPU_HISTORY", "1"); // M5: last draws/dispatches printed on a GPU crash
        mode = "entry";
    }
    if (mode == "entry-lut") {
        // Same as the no-tess run with the neutral default-colour LUT served for the asset the dump
        // lacks (bbport: a grey identity LUT is what that file is, so it cannot distort the image).
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_ASSET_PLACEHOLDER", "1");
        mode = "entry";
    }
    if (mode == "entry-mem-1q") {
        // Same as entry-mem with one graphics queue instead of two: tests whether the three 1 GiB
        // no-cache regions are one per queue (two graphics + the side queue).
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_VRAM_LOG", "1");
        _putenv_s("BB_GRAPHICS_QUEUES", "1");
        mode = "entry";
    }
    if (mode == "entry-mem-noguard") {
        // Same as entry-mem without the guard-page VkAllocationCallbacks: one VirtualAlloc per VMA
        // metadata allocation is a candidate for the private commit that grows outside the guest.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_VRAM_LOG", "1");
        _putenv_s("BB_GUARD_ALLOC", "0");
        mode = "entry";
    }
    if (mode == "entry-mem") {
        // Same as the no-tess run, with the memory split printed periodically (BB_VRAM_LOG): the
        // breakdown has to be known before the entry is extended, or the next peak cannot be blamed.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_VRAM_LOG", "1");
        mode = "entry";
    }
    if (mode == "entry-unknownpm4") {
        // Diagnostic: an opcode this build does not know is logged with its payload and skipped by its
        // own length instead of ending the run, so the whole stream can be mapped in one pass.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_UNKNOWN_PM4", "1");
        mode = "entry";
    }
    if (mode == "entry-dump") {
        // Same as the no-tess run, dumping every compiled shader so the refused compute program can
        // be validated offline (spirv-val) and diffed against one that creates.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_DUMP_SHADERS", "1");
        mode = "entry";
    }
    if (mode == "entry-pad") {
        // Same as the no-tess run plus one unused binding in every compute layout: isolates layout
        // shape from the shader as the reason a compute pipeline is refused.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        _putenv_s("BB_PAD_BINDINGS", "1");
        mode = "entry";
    }
    if (mode == "entry-notess-full") {
        // The missing cell of the 2x2: every optional device feature LEFT ON (so the pipeline key
        // keeps the full dynamic-state list) and only the tessellation emulation removed. Separates
        // "the ICD refuses tessellation" from "the ICD refuses the dynamic-state machinery".
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_NO_AUX_TESS", "1");
        mode = "entry";
    }
    if (mode == "entry-notess") {
        // Same as the static-PSO run, plus no synthesised tessellation for rect/quad lists: tells
        // whether the ICD refuses the 4-stage (vs+tcs+tes+fs) pipeline as such.
        _putenv_s("BB_REAL_GNM", "1");
        _putenv_s("BB_DRAW", "1");
        _putenv_s("BB_PREP_WORKERS", "0");
        _putenv_s("BB_FORCE_STATIC_PSO", "1");
        _putenv_s("BB_NO_AUX_TESS", "1");
        mode = "entry";
    }
    if (mode == "entry-flipverify") {
        // Diagnostic: let the first flip upload its guest snapshot through the real bridge and compare
        // the hashes. It blocks the flip thread on purpose, so it is never the default path.
        _putenv_s("BB_FLIP_VERIFY", "1");
        mode = "entry";
    }
    if (mode == "renderer-upload") {
        // Proves the upload bridge (host range -> device-local -> host readback) without a frame.
        _putenv_s("BB_UPLOAD_TEST", "1");
        mode = "renderer-boot";
    }
    inspect_dump(dump);
#ifdef UNCHARTED_BUILD_GRAPHICS
    if (mode == "probe") {
    try {
        std::puts("M2: starting Dozen/CoreWindow graphics probe");
        const int graphics = UnchartedUwp::GraphicsProbe();
        std::printf("M2 graphics probe returned %d\n", graphics);
#ifdef UNCHARTED_LINK_RENDERER
        std::printf("M2 bbgpu link smoke returned %d\n", uncharted_bbgpu_link_smoke());
#endif
    } catch (const std::exception& error) {
        std::printf("M2 graphics probe FAILED: %s\n", error.what());
        finished = true;
        return;
    }
    }
#endif

    const std::string local_utf8 = winrt::to_string(local);
    const std::string user = local_utf8 + "\\user";
    CreateDirectoryW(winrt::to_hstring(user).c_str(), nullptr);
    _putenv_s("BB_GPU_USER_DIR", user.c_str());
    _putenv_s("BB_GAME_SERIAL", "CUSA02320");
    _putenv_s("BB_UWP_SWAP_FILE", (local_utf8 + "\\uncharted-swap.bin").c_str());
#ifdef UNCHARTED_M5_HEADLESS_DEVICE
    // Diagnostic boot: no pipeline preload thread and no shader database, so the renderer
    // objects are built by a single thread. Both are only needed when a game is running.
    if (std::getenv("BB_WARM_FORCE") == nullptr) {
        _putenv_s("BB_WARM_MODE", "0");
    }
    // Report whether the guest frame buffers have real, writable memory behind them (the port has
    // no per-page residency bookkeeping, so the address alone proves nothing).
    _putenv_s("BB_VIDEOOUT_RESIDENCY", "1");
#endif
#ifdef UNCHARTED_M5_HEADLESS_DEVICE
    if (mode == "dll-boot") {
        HMODULE dll = LoadPackagedLibrary(L"bbprobe.dll", 0);
        if (!dll) {
            std::printf("M5 dll boot: LoadPackagedLibrary failed, error=%lu\n", GetLastError());
        } else {
            auto entry = reinterpret_cast<int (*)()>(GetProcAddress(dll, "bbgpu_dll_boot_smoke"));
            if (!entry) {
                std::printf("M5 dll boot: entry point missing, error=%lu\n", GetLastError());
            } else {
                std::printf("M5 dll boot returned %d\n", entry());
            }
            FreeLibrary(dll);
        }
    } else if (mode == "dll-host-instance") {
        // The executable owns the Vulkan instance; the DLL only receives the object.
        Vulkan::Instance host_instance{-1, false};
        std::printf("M5 dll host-instance: exe instance ready\n");
        std::fflush(stdout);
        HMODULE renderer_dll = LoadPackagedLibrary(L"bbgpu_dll.dll", 0);
        if (!renderer_dll) {
            std::printf("M5 dll host-instance: load failed, error=%lu\n", GetLastError());
        } else {
            using Entry = int (*)(void*, void*, void*);
            auto entry = reinterpret_cast<Entry>(
                GetProcAddress(renderer_dll, "bbgpu_dll_host_instance"));
            if (!entry) {
                std::printf("M5 dll host-instance: entry missing, error=%lu\n", GetLastError());
            } else {
                std::printf("M5 dll host-instance returned %d\n",
                            entry(&host_instance, &VULKAN_HPP_DEFAULT_DISPATCHER,
                            uncharted_uwp_memory_manager()));
            }
            FreeLibrary(renderer_dll);
        }
    } else if (mode == "dll-renderer") {
        HMODULE renderer_dll = LoadPackagedLibrary(L"bbgpu_dll.dll", 0);
        if (!renderer_dll) {
            std::printf("M5 dll renderer: load failed, error=%lu\n", GetLastError());
        } else {
            auto entry = reinterpret_cast<int (*)()>(
                GetProcAddress(renderer_dll, "bbgpu_dll_renderer_smoke"));
            if (!entry) {
                std::printf("M5 dll renderer: entry missing, error=%lu\n", GetLastError());
            } else {
                std::printf("M5 dll renderer returned %d\n", entry());
            }
            FreeLibrary(renderer_dll);
        }
    } else if (mode == "dll-probe") {
        HMODULE dll = LoadPackagedLibrary(L"bbprobe.dll", 0);
        if (!dll) {
            std::printf("M5 dll probe: LoadPackagedLibrary failed, error=%lu\n", GetLastError());
        } else {
            auto answer = reinterpret_cast<int (*)()>(GetProcAddress(dll, "bbprobe_answer"));
            auto add = reinterpret_cast<int (*)(int, int)>(GetProcAddress(dll, "bbprobe_add"));
            if (!answer || !add) {
                std::printf("M5 dll probe: GetProcAddress failed, error=%lu\n", GetLastError());
            } else {
                auto load = reinterpret_cast<int (*)()>(GetProcAddress(dll, "bbprobe_load_renderer"));
                std::printf("M5 dll probe PASS: answer=%d sum=%d renderer_load=%d\n", answer(),
                            add(20, 22), load ? load() : -1);
            }
            FreeLibrary(dll);
        }
    } else if (mode == "renderer-sem") {
        const int result=bbgpu_uwp_renderer_sem_smoke();
        std::printf("M5 renderer semaphore smoke returned %d\n",result);
    } else if (mode == "renderer-probe") {
        const int result=bbgpu_uwp_renderer_probe_smoke();
        std::printf("M5 renderer probe smoke returned %d\n",result);
    } else if (mode == "renderer-boot") {
        bb_log_raw("R3 exe-before-boot-call");
        const int result=bbgpu_uwp_renderer_boot_smoke();
        bb_log_raw("R4 exe-after-boot-call");
        std::printf("M5 renderer boot smoke returned %d\n",result);
        bb_log_raw("R5 exe-after-returned-print");
    } else if (mode == "renderer-swapchain") {
        const int result=bbgpu_uwp_renderer_swapchain_smoke();
        std::printf("M5 renderer swapchain smoke returned %d\n",result);
    } else if (mode == "renderer-hostpass") {
        const int result=bbgpu_uwp_renderer_hostpass_smoke();
        std::printf("M5 renderer host pass smoke returned %d\n",result);
    } else if (mode == "renderer-compute-empty") {
        const int result=bbgpu_uwp_renderer_compute_smoke();
        std::printf("M5 renderer empty compute smoke returned %d\n",result);
    } else if (mode == "renderer-descriptor") {
        const int result=bbgpu_uwp_renderer_descriptor_smoke();
        std::printf("M5 renderer descriptor smoke returned %d\n",result);
    } else if (mode == "renderer-offscreen") {
        const int result=bbgpu_uwp_renderer_offscreen_smoke();
        std::printf("M5 renderer offscreen smoke returned %d\n",result);
    } else if (mode == "renderer-class-device") {
        const int result=bbgpu_uwp_renderer_class_device_smoke();
        std::printf("M5 Vulkan::Instance class device returned %d\n",result);
    } else if (mode == "renderer-device") {
        const int result=bbgpu_uwp_headless_device_smoke();
        std::printf("M5 renderer Vulkan device smoke returned %d\n",result);
    } else
#endif
#ifdef UNCHARTED_M5_HEADLESS_INSTANCE
    if (mode == "renderer-instance") {
        const int result=bbgpu_uwp_headless_instance_smoke();
        std::printf("M5 renderer Vulkan instance smoke returned %d\n",result);
    } else
#endif
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
    if (mode == "video-contract") {
        const int result=bbgpu_uwp_videoout_contract_selftest();
        std::printf("M5 logical VideoOut contract self-test returned %d\n",result);
    } else if (mode == "gnm-event") {
        const int result=bbgpu_uwp_gnm_event_selftest();
        std::printf("M5 GNM event contract self-test returned %d\n",result);
    } else
#endif
#ifdef UNCHARTED_BUILD_GUEST
    if (mode == "equeue") {
        const int result=bbgpu_uwp_kernel_selftest();
        std::printf("M4 real kernel equeue self-test returned %d\n",result);
    } else
#endif
    if (mode == "memory") {
        std::string arg0="uncharted-uwp", test="--memory-selftest";
        char* args[]={arg0.data(),test.data()};
        const int result=uncharted_probe_main(2,args);
        std::printf("Memory contract self-test returned %d\n",result);
    } else if (!dump.empty() && mode == "stage") {
        std::puts("M3: staging minimal U1 preparation inputs from USB to LocalState");
        std::printf("M3 staging returned %d\n", stage_dump(dump, std::wstring(local)));
    } else if (!dump.empty() && (mode == "init" || mode == "entry" || mode == "loader" || mode == "entry-missing")) {
        const bool init_only=mode=="init";
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
        std::printf("M5 diagnostic: invoking Uncharted U1 %s with logical VideoOut, no guest renderer\n",
                    init_only ? "module initialization" : "controlled guest entry");
#elif defined(UNCHARTED_BUILD_GUEST)
        std::printf("M4: invoking Uncharted U1 %s with real kernel event queues in CPU-only mode\n",
                    init_only ? "module initialization" : "controlled guest entry");
#else
        std::printf("M3: invoking Uncharted U1 %s in CPU-only mode\n",init_only ? "module initialization" : "controlled guest entry");
#endif
        std::string boot=local_utf8+"\\Prepared\\boot-linked.bin";
        std::string content=local_utf8+"\\Prepared\\content.bin";
        std::string trophies=local_utf8+"\\Prepared\\trophies";
        // The console portal cannot create a subdirectory inside LocalState (POST to
        // LocalState/Prepared/... returns 500), so the prepared image may have been uploaded flat.
        if (!readable(boot) || !readable(content)) {
            const std::string flat_boot = local_utf8 + "\\boot-linked.bin";
            const std::string flat_content = local_utf8 + "\\content.bin";
            if (readable(flat_boot) && readable(flat_content)) {
                boot = flat_boot;
                content = flat_content;
                trophies = local_utf8 + "\\trophies";
                std::puts("M5 diagnostic: using the flat prepared layout in LocalState");
            }
        }
        // Prerequisites of the trace itself: the guest path is only meaningful with the image
        // present, so record which files and sizes the run actually used.
        {
            WIN32_FILE_ATTRIBUTE_DATA boot_data{};
            WIN32_FILE_ATTRIBUTE_DATA content_data{};
            const bool boot_ok = GetFileAttributesExFromAppW(winrt::to_hstring(boot).c_str(),
                                                             GetFileExInfoStandard, &boot_data) != 0;
            const bool content_ok = GetFileAttributesExFromAppW(
                                        winrt::to_hstring(content).c_str(), GetFileExInfoStandard,
                                        &content_data) != 0;
            std::printf("M5 guest image: boot=%s (%llu bytes) content=%s (%llu bytes)\n",
                        boot_ok ? "present" : "MISSING",
                        static_cast<unsigned long long>(boot_ok ? boot_data.nFileSizeLow : 0),
                        content_ok ? "present" : "MISSING",
                        static_cast<unsigned long long>(content_ok ? content_data.nFileSizeLow : 0));
            std::fflush(stdout);
        }
        if (!readable(boot) || !readable(content)) {
            std::puts("M3 FAILED: Prepared/boot-linked.bin or content.bin is missing");
        } else {
            std::vector<std::string> values={"uncharted-uwp",boot,"--cpu-only","--app0",dump,
                "--user",user,"--content-profile",content,"--trophies",trophies};
            if (init_only) values.emplace_back("--init-only");
            // Diagnostic: list every import the HLE does not resolve, instead of discovering one per run.
            if (mode == "entry-missing") values.emplace_back("--list-missing");
            std::vector<char*> args; for (auto& value:values) args.push_back(value.data());
            runtime_memory_start_paging();
            const int result=uncharted_probe_main(static_cast<int>(args.size()),args.data());
#ifdef UNCHARTED_M5_VIDEO_CONTRACT
            std::printf("M5 diagnostic Uncharted loader returned %d\n",result);
#elif defined(UNCHARTED_BUILD_GUEST)
            std::printf("M4 Uncharted loader returned %d\n",result);
#else
            std::printf("M3 Uncharted loader returned %d\n",result);
#endif
        }
    } else {
        std::puts("M2 READY: graphics host complete; select M3 stage, memory, init or entry explicitly.");
    }
    bb_log_raw("L8 before-startup-memory");
    std::printf("UWP startup memory: %llu/%llu MiB\n",
                winrt::Windows::System::MemoryManager::AppMemoryUsage() >> 20,
                winrt::Windows::System::MemoryManager::AppMemoryUsageLimit() >> 20);
    finished = true;
    bb_log_raw("L-end loader-returning");
}

struct View : winrt::implements<View, winrt::Windows::ApplicationModel::Core::IFrameworkView> {
    winrt::Windows::UI::Core::CoreWindow window{nullptr};
    void Initialize(const winrt::Windows::ApplicationModel::Core::CoreApplicationView& app) {
        app.Activated([this](auto const&, auto const&) { window.Activate(); });
    }
    void SetWindow(const winrt::Windows::UI::Core::CoreWindow& value) {
        window = value;
        // The DLL enters CoreApplication::Run after the packaged host is already activated, so the
        // Activated callback may not be replayed. Activate here as well to put this CoreWindow in front.
        window.Activate();
        core_window = winrt::make_agile(window);
        uncharted_uwp_set_core_window(
            reinterpret_cast<IUnknown*>(winrt::get_abi(core_window.get())));
        window.Closed([](auto const&, auto const&) { window_closed = true; });
        // On Xbox, B is the system Back gesture: an app that does not claim it is navigated out and
        // the game disappears to the dashboard. The game reads its own pad state, so the port only
        // has to keep the shell from acting on it.
        try {
            auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
            navigation.BackRequested([](auto const&, auto const& args) { args.Handled(true); });
            std::puts("M2: system Back request claimed (B no longer leaves the game)");
        } catch (...) {
            std::puts("M2: system Back request could not be claimed");
        }
    }
    void Load(const winrt::hstring&) {}
    void Run() {
        // The renderer construction is deep. std::thread uses the default 1 MiB stack, and an
        // overflow there kills the process without any exception reaching the vectored handler
        // (the handler cannot run on an exhausted stack) - the diagnostic boot then "vanished"
        // at a different depth on every run. The executable already reserves 16 MiB; give the
        // loader thread the same.
        HANDLE worker = CreateThread(
            nullptr, 16u << 20,
            [](void*) -> DWORD {
                loader();
                return 0;
            },
            nullptr, 0, nullptr);
        while (!window_closed) {
            window.Dispatcher().ProcessEvents(finished ?
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessOneAndAllPending :
                winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(finished ? 50 : 10);
        }
        if (worker != nullptr) {
            WaitForSingleObject(worker, INFINITE);
            CloseHandle(worker);
        }
    }
    void Uninitialize() {}
};

struct ViewSource : winrt::implements<ViewSource, winrt::Windows::ApplicationModel::Core::IFrameworkViewSource> {
    winrt::Windows::ApplicationModel::Core::IFrameworkView CreateView() { return winrt::make<View>(); }
};

// Exported entry used when the application runs from bbgpu.dll (the executable is only a host).
// It writes entry.log before touching WinRT so an early crash still leaves evidence, and it
// tolerates an already-initialized apartment (UWP UI thread is an ASTA).
extern "C" void bb_renderer_set_host_instance(void* instance);
extern "C" int bb_vma_set_handles(VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr,
                                  PFN_vkGetDeviceProcAddr);

extern "C" __declspec(dllexport) int bbgpu_uwp_app_main(void* host_instance,
                                                        void* host_dispatcher) {
    if (host_dispatcher != nullptr) {
        VULKAN_HPP_DEFAULT_DISPATCHER =
            *reinterpret_cast<VULKAN_HPP_DEFAULT_DISPATCHER_TYPE*>(host_dispatcher);
    }
    // The loader thread has no WinRT yet, so the raw marker channel gets its path from here — the
    // UI thread, which already writes entry.log and is known to work.
    try {
        const auto raw_local =
            winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
        const std::string raw_path = winrt::to_string(raw_local) + "\\boot.log";
        _putenv_s("BB_RAW_LOG", raw_path.c_str());
    } catch (...) {
    }
    bb_renderer_set_host_instance(host_instance);
    if (host_instance != nullptr) {
        auto& host = *static_cast<Vulkan::Instance*>(host_instance);
        const int vma_result = bb_vma_set_handles(
            static_cast<VkInstance>(host.GetInstance()),
            static_cast<VkPhysicalDevice>(host.GetPhysicalDevice()),
            static_cast<VkDevice>(host.GetDevice()),
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
            VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
        std::printf("M5 entry: local vma=%d\n", vma_result);
        std::fflush(stdout);
    }
    try {
        const auto local = winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path();
        if (FILE* entry = _wfopen((std::wstring(local) + L"\\entry.log").c_str(), L"w")) {
            std::fprintf(entry, "entry: build %s %s\n", __DATE__, __TIME__);
            std::fclose(entry);
        }
    } catch (...) {
    }
    try {
        winrt::init_apartment();
    } catch (const winrt::hresult_error&) {
    }
    // Single-module path: when the host does not create the instance, create it here on the UI
    // thread (the same thread that used to own it in the executable) and hand it to the loader
    // thread. Everything - instance, dispatcher, VMA and renderer - then lives in this module.
    static std::unique_ptr<Vulkan::Instance> owned_instance;
    if (host_instance == nullptr) {
        try {
            owned_instance = std::make_unique<Vulkan::Instance>(-1, false);
            bb_renderer_set_host_instance(owned_instance.get());
            if (FILE* entry =
                    _wfopen((std::wstring(winrt::Windows::Storage::ApplicationData::Current()
                                              .LocalFolder()
                                              .Path()) +
                             L"\\entry.log")
                                .c_str(),
                            L"a")) {
                std::fprintf(entry, "entry: DLL-owned Vulkan instance created on the UI thread\n");
                std::fclose(entry);
            }
        } catch (const std::exception& error) {
            if (FILE* entry =
                    _wfopen((std::wstring(winrt::Windows::Storage::ApplicationData::Current()
                                              .LocalFolder()
                                              .Path()) +
                             L"\\entry.log")
                                .c_str(),
                            L"a")) {
                std::fprintf(entry, "entry: DLL-owned instance FAILED: %s\n", error.what());
                std::fclose(entry);
            }
            return 3;
        }
    }
    winrt::Windows::ApplicationModel::Core::CoreApplication::Run(winrt::make<ViewSource>());
    return 0;
}
