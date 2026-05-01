#pragma once

// All clew resources (clew.log, clew.json, dns_state.json, frontend/dist)
// must resolve against the directory containing clew.exe — not the process
// cwd, which is unpredictable (Task Scheduler launches with cwd=system32,
// Explorer double-click uses the desktop folder, shell uses whatever the
// user happens to be in). These helpers wrap GetModuleFileNameW once so
// every consumer goes through the same code path.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <string>

namespace clew {

// Absolute path of the running clew.exe. Falls back to an empty path in
// the unlikely case GetModuleFileNameW fails — Windows itself only fails
// this for invalid module handles, which can't happen for the current
// process.
[[nodiscard]] inline std::filesystem::path exe_path() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path{std::wstring{buf, len}};
}

// Directory portion of the running clew.exe path. Falls back to "." if
// exe_path() failed.
[[nodiscard]] inline std::filesystem::path exe_directory() {
    auto p = exe_path();
    return p.empty() ? std::filesystem::path{L"."} : p.parent_path();
}

// Resolve a sibling resource of clew.exe by name. e.g. exe_relative("clew.log")
// → "C:\Users\foo\Downloads\clew\clew.log".
[[nodiscard]] inline std::filesystem::path exe_relative(std::wstring_view name) {
    return exe_directory() / name;
}

[[nodiscard]] inline std::filesystem::path exe_relative(std::string_view name) {
    return exe_directory() / name;
}

} // namespace clew
