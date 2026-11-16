// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

namespace Common {

template <class T>
class Singleton {
public:
    static T* Instance() {
        // Function-local state: no dynamic initializer runs at module load, so the order of
        // .CRT$XCU can no longer change behaviour between runs.
        static std::unique_ptr<T> instance;
        if (!instance) {
            instance = std::make_unique<T>();
        }
        return instance.get();
    }

protected:
    Singleton();
    ~Singleton();

private:

};

} // namespace Common
