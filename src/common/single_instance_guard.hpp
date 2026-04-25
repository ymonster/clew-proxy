#pragma once

// RAII guard for "single-instance" GUI apps. Ctor creates a named mutex and
// records whether another process already held it. If a duplicate launch is
// detected, the caller can use activate_existing() to bring the running
// window forward instead of opening a second one.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "core/scoped_exit.hpp"

namespace clew {

class single_instance_guard {
public:
    explicit single_instance_guard(const wchar_t* mutex_name) noexcept
        : mutex_(CreateMutexW(nullptr, TRUE, mutex_name)) {
        already_running_ = (GetLastError() == ERROR_ALREADY_EXISTS);
    }

    [[nodiscard]] bool already_running() const noexcept { return already_running_; }

    // Best-effort: bring the existing window forward.
    void activate_existing(const wchar_t* window_class_name) const noexcept {
        HWND existing = FindWindowW(window_class_name, nullptr);
        if (!existing) return;
        ShowWindow(existing, SW_SHOW);
        ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
    }

    single_instance_guard(const single_instance_guard&)            = delete;
    single_instance_guard& operator=(const single_instance_guard&) = delete;

private:
    unique_handle mutex_;
    bool          already_running_ = false;
};

} // namespace clew
