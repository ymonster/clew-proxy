#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <memory>
#include <utility>

namespace clew {

// RAII wrapper for Win32 HANDLE.
// - Constructed from an API return value. If the value is INVALID_HANDLE_VALUE
//   the object stays empty (no cleanup call later) — call .get() to retrieve
//   the raw handle for API calls, and test truthiness of the unique_handle to
//   know whether the underlying API call succeeded.
// - The deleter no-ops on nullptr / INVALID_HANDLE_VALUE, so constructing
//   from a still-raw "might-have-failed" HANDLE is safe.
struct handle_deleter {
    using pointer = HANDLE;
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using unique_handle = std::unique_ptr<void, handle_deleter>;

// Construct from a raw HANDLE. Returns an empty unique_handle if the call
// returned INVALID_HANDLE_VALUE or nullptr (i.e. the API failed).
inline unique_handle wrap_handle(HANDLE h) noexcept {
    if (h == nullptr || h == INVALID_HANDLE_VALUE) return {};
    return unique_handle{h};
}

// Generic scope-guard: runs `fn` on destruction unless release() is called.
// Use for cleanup of non-HANDLE resources (GDI objects, HKEY, HMODULE,
// third-party opaque pointers, etc.) where bespoke unique_ptr deleters
// would be heavier than a local lambda.
template<class F>
class scoped_exit {
    F fn_;
    bool armed_ = true;
public:
    explicit scoped_exit(F fn) noexcept : fn_(std::move(fn)) {}
    ~scoped_exit() { if (armed_) fn_(); }
    void release() noexcept { armed_ = false; }

    scoped_exit(const scoped_exit&) = delete;
    scoped_exit& operator=(const scoped_exit&) = delete;
    scoped_exit(scoped_exit&&) = delete;
    scoped_exit& operator=(scoped_exit&&) = delete;
};

template<class F> scoped_exit(F) -> scoped_exit<F>;

} // namespace clew
