// SPDX-FileCopyrightText: 2013 Dolphin Emulator Project
// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/logging/log.h"

// Sometimes we want to try to continue even after hitting an assert.
// However touching this file yields a global recompilation as this header is included almost
// everywhere. So let's just move the handling of the failed assert to a single cpp file.

void assert_fail_impl();
[[noreturn]] void unreachable_impl();

#ifdef _MSC_VER
#define SHAD_NO_INLINE __declspec(noinline)
#else
#define SHAD_NO_INLINE __attribute__((noinline))
#endif

// bbport: LOG_CRITICAL goes to the logger (stderr), which is not a valid channel in the UWP
// AppContainer, so an assert used to die silently. This writes file:line through the raw channel
// before the break, which is what makes the assert text readable from the console log.
extern "C" void bb_assert_marker(const char* file, int line, const char* kind);

// The macros below never called it, so a console assert still arrived with no location: the counter
// said "GPU library assertion failed" and nothing else. Only on UWP, where the marker exists.
#ifdef UNCHARTED_UWP
#define BB_ASSERT_LOCATION(kind) bb_assert_marker(__FILE__, __LINE__, kind)
#else
#define BB_ASSERT_LOCATION(kind) ((void)0)
#endif

#define ASSERT(_a_)                                                                                \
    do {                                                                                           \
        if (!(_a_)) [[unlikely]] {                                                                 \
            BB_ASSERT_LOCATION("assert");                                                          \
            LOG_CRITICAL(Debug, "Assertion Failed!");                                              \
            assert_fail_impl();                                                                    \
        }                                                                                          \
    } while (false)

#define ASSERT_MSG(_a_, ...)                                                                       \
    do {                                                                                           \
        if (!(_a_)) [[unlikely]] {                                                                 \
            BB_ASSERT_LOCATION("assert-msg");                                                      \
            LOG_CRITICAL(Debug, "Assertion Failed!\n" __VA_ARGS__);                                \
            assert_fail_impl();                                                                    \
        }                                                                                          \
    } while (false)

#define UNREACHABLE()                                                                              \
    do {                                                                                           \
        BB_ASSERT_LOCATION("unreachable");                                                         \
        LOG_CRITICAL(Debug, "Unreachable code!");                                                  \
        unreachable_impl();                                                                        \
    } while (0)

#define UNREACHABLE_MSG(...)                                                                       \
    do {                                                                                           \
        BB_ASSERT_LOCATION("unreachable-msg");                                                     \
        LOG_CRITICAL(Debug, "Unreachable code!\n" __VA_ARGS__);                                    \
        unreachable_impl();                                                                        \
    } while (0)

#ifdef _DEBUG
#define DEBUG_ASSERT(_a_) ASSERT(_a_)
#define DEBUG_ASSERT_MSG(_a_, ...) ASSERT_MSG(_a_, __VA_ARGS__)
#else // not debug
#define DEBUG_ASSERT(_a_)                                                                          \
    do {                                                                                           \
    } while (0)
#define DEBUG_ASSERT_MSG(_a_, _desc_, ...)                                                         \
    do {                                                                                           \
    } while (0)
#endif

#define UNIMPLEMENTED() ASSERT_MSG(false, "Unimplemented code!")
#define UNIMPLEMENTED_MSG(...) ASSERT_MSG(false, __VA_ARGS__)

#define UNIMPLEMENTED_IF(cond) ASSERT_MSG(!(cond), "Unimplemented code!")
#define UNIMPLEMENTED_IF_MSG(cond, ...) ASSERT_MSG(!(cond), __VA_ARGS__)

// If the assert is ignored, execute _b_
#define ASSERT_OR_EXECUTE(_a_, _b_)                                                                \
    do {                                                                                           \
        ASSERT(_a_);                                                                               \
        if (!(_a_)) [[unlikely]] {                                                                 \
            _b_                                                                                    \
        }                                                                                          \
    } while (0)

// If the assert is ignored, execute _b_
#define ASSERT_OR_EXECUTE_MSG(_a_, _b_, ...)                                                       \
    do {                                                                                           \
        ASSERT_MSG(_a_, __VA_ARGS__);                                                              \
        if (!(_a_)) [[unlikely]] {                                                                 \
            _b_                                                                                    \
        }                                                                                          \
    } while (0)
