#pragma once

// RAII guard for debug-mode console attachment in a Windows GUI app.
// Ctor allocates a console and redirects stdout/stderr to it; dtor frees it.
// Use under #ifndef NDEBUG so release builds skip both the allocation and
// the dependency on stdio redirection.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cstdio>
#include <windows.h>

namespace clew {

class debug_console_session {
public:
    debug_console_session() noexcept {
        AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }

    ~debug_console_session() noexcept {
        FreeConsole();
    }

    debug_console_session(const debug_console_session&)            = delete;
    debug_console_session& operator=(const debug_console_session&) = delete;
};

} // namespace clew
