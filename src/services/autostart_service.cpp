#include "services/autostart_service.hpp"

#include <array>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "common/api_exception.hpp"
#include "core/exe_paths.hpp"
#include "core/log.hpp"
#include "core/scoped_exit.hpp"

namespace clew {
namespace {

namespace fs = std::filesystem;

constexpr std::wstring_view TASK_NAME = L"ClewAutoStart";
constexpr std::string_view  MINIMIZED_FLAG = "--minimized";
constexpr DWORD             PROCESS_NOT_STARTED = std::numeric_limits<DWORD>::max();

struct schtasks_result {
    DWORD       exit_code     = PROCESS_NOT_STARTED;
    DWORD       win32_error   = ERROR_SUCCESS;
    std::string stdout_text;
    std::string stderr_text;

    [[nodiscard]] bool launched() const noexcept {
        return win32_error == ERROR_SUCCESS;
    }

    [[nodiscard]] bool succeeded() const noexcept {
        return launched() && exit_code == 0;
    }

    [[nodiscard]] std::string output_text() const {
        if (stdout_text.empty()) return stderr_text;
        if (stderr_text.empty()) return stdout_text;
        return stdout_text + '\n' + stderr_text;
    }
};

std::string trim_trailing_crlf(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

std::string win32_message(DWORD code) {
    char* raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;

    const DWORD len = FormatMessageA(
        flags,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&raw),
        0,
        nullptr);

    if (len == 0 || raw == nullptr) {
        return std::format("Win32 error {}", code);
    }

    std::string msg{raw, len};
    LocalFree(raw);
    return std::format("Win32 error {}: {}", code, trim_trailing_crlf(std::move(msg)));
}

std::string win32_failure(std::string_view operation, DWORD code = GetLastError()) {
    return std::format("{} failed ({})", operation, win32_message(code));
}

std::string read_all(HANDLE h) {
    std::string out;
    std::array<char, 4096> chunk{};

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)) {
            break;
        }
        if (got == 0) {
            break;
        }
        out.append(chunk.data(), got);
    }

    return out;
}

// Quote one argument according to the parsing rules used by CommandLineToArgvW
// and the Microsoft C/C++ runtime. This is intentionally argument-level quoting;
// callers still decide which logical arguments should be grouped together.
std::wstring quote_win_arg(std::wstring_view arg) {
    if (arg.empty()) {
        return L"\"\"";
    }

    const bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needs_quotes) {
        return std::wstring{arg};
    }

    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');

    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
        } else {
            out.append(backslashes, L'\\');
            out.push_back(ch);
        }

        backslashes = 0;
    }

    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring make_command_line(const fs::path& executable,
                               std::initializer_list<std::wstring_view> args) {
    std::wstring cmdline = quote_win_arg(executable.native());

    for (const std::wstring_view arg : args) {
        cmdline.push_back(L' ');
        cmdline += quote_win_arg(arg);
    }

    return cmdline;
}

fs::path system32_directory() {
    std::array<wchar_t, MAX_PATH> buf{};
    const UINT len = GetSystemDirectoryW(buf.data(), static_cast<UINT>(buf.size()));

    if (len == 0 || len >= buf.size()) {
        return {};
    }

    return fs::path{std::wstring{buf.data(), len}};
}

fs::path schtasks_path() {
    const fs::path system32 = system32_directory();
    if (system32.empty()) {
        return L"schtasks.exe";
    }
    return system32 / L"schtasks.exe";
}

schtasks_result run_schtasks(std::initializer_list<std::wstring_view> args) {
    schtasks_result result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_read_raw  = nullptr;
    HANDLE out_write_raw = nullptr;
    HANDLE err_read_raw  = nullptr;
    HANDLE err_write_raw = nullptr;

    if (!CreatePipe(&out_read_raw, &out_write_raw, &sa, 0)) {
        result.win32_error = GetLastError();
        result.stderr_text = win32_failure("CreatePipe(stdout)", result.win32_error);
        return result;
    }
    unique_handle out_read{out_read_raw};
    unique_handle out_write{out_write_raw};

    if (!CreatePipe(&err_read_raw, &err_write_raw, &sa, 0)) {
        result.win32_error = GetLastError();
        result.stderr_text = win32_failure("CreatePipe(stderr)", result.win32_error);
        return result;
    }
    unique_handle err_read{err_read_raw};
    unique_handle err_write{err_write_raw};

    // Only the pipe write ends should be inherited by schtasks.exe. If the child
    // inherits a read end, the parent can block forever waiting for EOF.
    if (!SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        result.win32_error = GetLastError();
        result.stderr_text = win32_failure("SetHandleInformation(stdout read)", result.win32_error);
        return result;
    }

    if (!SetHandleInformation(err_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        result.win32_error = GetLastError();
        result.stderr_text = win32_failure("SetHandleInformation(stderr read)", result.win32_error);
        return result;
    }

    STARTUPINFOW si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = out_write.get();
    si.hStdError  = err_write.get();
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};

    const fs::path schtasks = schtasks_path();
    std::wstring cmdline = make_command_line(schtasks, args);

    const wchar_t* app_name = schtasks.is_absolute() ? schtasks.c_str() : nullptr;

    const BOOL created = CreateProcessW(
        /*lpApplicationName=*/    app_name,
        /*lpCommandLine=*/        cmdline.data(),
        /*lpProcessAttributes=*/  nullptr,
        /*lpThreadAttributes=*/   nullptr,
        /*bInheritHandles=*/      TRUE,
        /*dwCreationFlags=*/      CREATE_NO_WINDOW,
        /*lpEnvironment=*/        nullptr,
        /*lpCurrentDirectory=*/   nullptr,
        /*lpStartupInfo=*/        &si,
        /*lpProcessInformation=*/ &pi);

    if (!created) {
        result.win32_error = GetLastError();
        result.stderr_text = win32_failure("CreateProcessW(schtasks.exe)", result.win32_error);
        return result;
    }

    unique_handle process{pi.hProcess};
    unique_handle thread{pi.hThread};

    // The parent must close its write handles before waiting for reader EOF.
    out_write.reset();
    err_write.reset();

    std::string stdout_text;
    std::string stderr_text;

    std::jthread stdout_reader{[&] {
        stdout_text = read_all(out_read.get());
    }};
    std::jthread stderr_reader{[&] {
        stderr_text = read_all(err_read.get());
    }};

    const DWORD wait_result = WaitForSingleObject(process.get(), INFINITE);
    if (wait_result == WAIT_FAILED) {
        result.win32_error = GetLastError();
        result.stderr_text = win32_failure("WaitForSingleObject(schtasks.exe)", result.win32_error);
    } else if (!GetExitCodeProcess(process.get(), &result.exit_code)) {
        result.win32_error = GetLastError();
        result.stderr_text = win32_failure("GetExitCodeProcess(schtasks.exe)", result.win32_error);
    }

    stdout_reader.join();
    stderr_reader.join();

    result.stdout_text = std::move(stdout_text);
    if (!stderr_text.empty()) {
        if (!result.stderr_text.empty()) result.stderr_text += '\n';
        result.stderr_text += std::move(stderr_text);
    }

    return result;
}

std::string ascii_to_utf16le_bytes(std::string_view text) {
    std::string out;
    out.reserve(text.size() * 2);

    for (const char ch : text) {
        out.push_back(ch);
        out.push_back('\0');
    }

    return out;
}

bool contains_ascii_or_utf16le(std::string_view bytes, std::string_view needle) {
    if (bytes.contains(needle)) {
        return true;
    }

    const std::string wide_needle = ascii_to_utf16le_bytes(needle);
    return bytes.contains(wide_needle);
}

bool output_contains_minimized_flag(const schtasks_result& result) {
    return contains_ascii_or_utf16le(result.stdout_text, MINIMIZED_FLAG) ||
           contains_ascii_or_utf16le(result.stderr_text, MINIMIZED_FLAG);
}

bool output_indicates_task_not_found(const schtasks_result& result) {
    const std::string output = result.output_text();

    return contains_ascii_or_utf16le(output, "cannot find") ||
           contains_ascii_or_utf16le(output, "does not exist") ||
           contains_ascii_or_utf16le(output, "The system cannot find") ||
           contains_ascii_or_utf16le(output, "The specified task name") ||
           output.contains("找不到");
}

std::string schtasks_failure_message(std::string_view operation, const schtasks_result& result) {
    std::string detail = result.output_text();
    if (detail.empty() && !result.launched()) {
        detail = win32_message(result.win32_error);
    }
    if (detail.empty()) {
        detail = std::format("exit_code={}", result.exit_code);
    }

    return std::format("{} failed: {}", operation, detail);
}

std::wstring make_task_action(bool start_minimized) {
    const fs::path exe = exe_path();
    if (exe.empty()) {
        throw api_exception{api_error::internal, "GetModuleFileNameW failed"};
    }

    const fs::path config = exe_relative(L"clew.json");

    std::wstring action = quote_win_arg(exe.native()) +
                          L" --config " +
                          quote_win_arg(config.native());

    if (start_minimized) {
        action += L" --minimized";
    }

    return action;
}

} // namespace

autostart_state autostart_service::query() {
    autostart_state state;

    const schtasks_result result = run_schtasks({L"/Query", L"/TN", TASK_NAME, L"/XML"});
    if (!result.succeeded()) {
        if (!output_indicates_task_not_found(result)) {
            PC_LOG_INFO("autostart: schtasks /Query failed: {}",
                        schtasks_failure_message("schtasks /Query", result));
        }
        return state;
    }

    state.enabled = true;
    state.start_minimized = output_contains_minimized_flag(result);
    return state;
}

void autostart_service::set(bool enabled, bool start_minimized) {
    if (!enabled) {
        const schtasks_result result = run_schtasks({L"/Delete", L"/TN", TASK_NAME, L"/F"});

        if (!result.succeeded() && !output_indicates_task_not_found(result)) {
            throw api_exception{api_error::io_error,
                                schtasks_failure_message("schtasks /Delete", result)};
        }

        PC_LOG_INFO("autostart: ClewAutoStart task removed");
        return;
    }

    const std::wstring task_action = make_task_action(start_minimized);

    const schtasks_result result = run_schtasks({
        L"/Create",
        L"/TN", TASK_NAME,
        L"/TR", task_action,
        L"/SC", L"ONLOGON",
        L"/RL", L"HIGHEST",
        L"/F",
    });

    if (!result.succeeded()) {
        throw api_exception{api_error::io_error,
                            schtasks_failure_message("schtasks /Create", result)};
    }

    PC_LOG_INFO("autostart: ClewAutoStart task created (start_minimized={})",
                start_minimized);
}

} // namespace clew
