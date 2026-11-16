// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: dumping shaders/IR is a user setting, and the UWP shell has no settings UI at all, so a
// console run cannot ask for it. This gate reads the request from the environment instead:
//   BB_DUMP_SHADERS=1            dump every program (expensive: the whole load, thousands of files)
//   BB_DUMP_SHADERS=0x6a6422a9,…  dump only these programs (cheap; what the crash history blamed)
// Hashes are compared on the full 64-bit value and on its low 32 bits, because the diagnostic prints
// show the low half when the high half is zero. Unset or "0" keeps the old behaviour exactly.
#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace BbPort {

struct DumpFilter {
    bool all;
    bool enabled;
    unsigned long long hashes[16];
    std::size_t count;
};

inline const DumpFilter& GetDumpFilter() {
    static const DumpFilter filter = [] {
        DumpFilter f{};
        const char* env = std::getenv("BB_DUMP_SHADERS");
        if (env == nullptr || env[0] == '\0' || (env[0] == '0' && env[1] == '\0')) {
            return f;
        }
        f.enabled = true;
        if (env[0] == '1' && env[1] == '\0') {
            f.all = true;
            return f;
        }
        const char* p = env;
        while (*p != '\0' && f.count < 16) {
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                f.hashes[f.count++] = std::strtoull(p + 2, nullptr, 16);
            }
            while (*p != '\0' && *p != ',') {
                ++p;
            }
            if (*p == ',') {
                ++p;
            }
        }
        return f;
    }();
    return filter;
}

/// Prints the parsed filter once, so a run says whether the dump request arrived at all.
inline void LogDumpFilterOnce() {
    static const bool once = [] {
        const DumpFilter& filter = GetDumpFilter();
        std::printf("M5 dump gate: enabled=%d all=%d hashes=%zu\n", static_cast<int>(filter.enabled),
                    static_cast<int>(filter.all), filter.count);
        return true;
    }();
    (void)once;
}

inline bool DumpShadersFor(unsigned long long hash) {    const DumpFilter& filter = GetDumpFilter();
    if (!filter.enabled) {
        return false;
    }
    if (filter.all) {
        return true;
    }
    for (std::size_t i = 0; i < filter.count; ++i) {
        if (filter.hashes[i] == hash ||
            static_cast<unsigned>(filter.hashes[i]) == static_cast<unsigned>(hash)) {
            return true;
        }
    }
    return false;
}

} // namespace BbPort
