// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#ifdef UNCHARTED_UWP
// Raw marker channel (one WriteFile); declared at namespace scope because a linkage specification
// is not allowed at block scope.
extern "C" void bb_log_raw(const char* text);
#endif

namespace Common {

/// General purpose function wrapper similar to std::function.
/// Unlike std::function, the captured values don't have to be copyable.
/// This class can be moved but not copied.
template <typename ResultType, typename... Args>
class UniqueFunction {
    class CallableBase {
    public:
        virtual ~CallableBase() = default;
        virtual ResultType operator()(Args&&...) = 0;
    };

    template <typename Functor>
    class Callable final : public CallableBase {
    public:
        Callable(Functor&& functor_) : functor{std::move(functor_)} {}
        ~Callable() override = default;

        ResultType operator()(Args&&... args) override {
            return functor(std::forward<Args>(args)...);
        }

    private:
        Functor functor;
    };

public:
    UniqueFunction() = default;

    template <typename Functor>
    UniqueFunction(Functor&& functor)
        : callable{std::make_unique<Callable<Functor>>(std::move(functor))} {}

    UniqueFunction& operator=(UniqueFunction&& rhs) noexcept = default;
    UniqueFunction(UniqueFunction&& rhs) noexcept = default;

    UniqueFunction& operator=(const UniqueFunction&) = delete;
    UniqueFunction(const UniqueFunction&) = delete;

    ResultType operator()(Args&&... args) const {
        /* bbport: a default-constructed or moved-from UniqueFunction has a null callable, and
         * `(*callable)(...)` on a null unique_ptr loads the vtable from address 0 — which is the
         * "call to 0x0, no stack" fault seen on the console. Callers in the scheduler defer
         * operations and may invoke an entry that was never set, so a null target is a no-op with a
         * marker instead of a jump to zero. */
        if (!callable) {
#ifdef UNCHARTED_UWP
            bb_log_raw("M5 UniqueFunction: null callable invoked (skipped)");
#endif
            if constexpr (!std::is_void_v<ResultType>) {
                return ResultType{};
            } else {
                return;
            }
        }
        return (*callable)(std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(callable);
    }

private:
    std::unique_ptr<CallableBase> callable;
};

} // namespace Common
