// Clew — Process-level Traffic Hijacker
// main.cpp: thin entry point. Pre-app guards (single-instance, debug
// console, logger, elevation, Winsock) live here; everything else is in
// clew::app (see app.hpp).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <WinSock2.h>
#include <Windows.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include "app.hpp"
#include "common/debug_console_session.hpp"
#include "common/single_instance_guard.hpp"
#include "common/winsock_session.hpp"
#include "core/log.hpp"
#include "core/scoped_exit.hpp"
#include "core/version.hpp"

// ---------------------------------------------------------------------------

static bool is_elevated() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return false;
    clew::unique_handle token(raw_token);

    TOKEN_ELEVATION elevation;
    if (DWORD size = 0; !GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size))
        return false;
    return elevation.TokenIsElevated == TRUE;
}

static void print_usage() {
    std::cout << "Clew " CLEW_VERSION " - Process-level Traffic Hijacker\n\n";
    std::cout << "Usage: clew [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --gui           Launch with WebView2 GUI\n";
    std::cout << "  --devtools      Enable WebView2 DevTools (F12)\n";
    std::cout << "  --static-dir    Path to static files directory\n";
    std::cout << "  --help, -h      Show this help message\n\n";
}

static clew::cli_options parse_args(int argc, char* argv[]) {
    clew::cli_options opts;
    std::span         args(argv, argc);
    auto              it = args.begin() + 1;
    while (it != args.end()) {
        std::string_view arg = *it++;
        if (arg == "--gui") opts.gui_mode = true;
        else if (arg == "--devtools") opts.devtools = true;
        else if (arg == "--static-dir" && it != args.end()) opts.static_dir = *it++;
        else if (arg == "--help" || arg == "-h") opts.help = true;
    }
    return opts;
}

static void setup_logger() {
    quill::Backend::start();

    auto file_sink = quill::Frontend::create_or_get_sink<quill::RotatingFileSink>(
        "clew.log",
        []() {
            quill::RotatingFileSinkConfig cfg;
            cfg.set_open_mode('w');
            cfg.set_rotation_max_file_size(50 * 1024 * 1024);
            cfg.set_max_backup_files(5);
            return cfg;
        }());

    quill::PatternFormatterOptions pattern{
        "%(time) [%(log_level_short_code)] %(message)",
        "%Y-%m-%d %H:%M:%S.%Qus"};

#ifndef NDEBUG
    auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
    clew::g_logger = quill::Frontend::create_or_get_logger(
        "clew", {std::move(file_sink), std::move(console_sink)}, pattern);
#else
    clew::g_logger = quill::Frontend::create_or_get_logger(
        "clew", std::move(file_sink), pattern);
#endif
}

static constexpr const wchar_t* CLEW_MUTEX_NAME   = L"Global\\Clew_SingleInstance";
static constexpr const wchar_t* CLEW_WINDOW_CLASS = L"ClewWebViewClass";

static int run_app(int argc, char** argv, HINSTANCE hinstance, bool default_gui_mode) {
    clew::cli_options opts = parse_args(argc, argv);
    if (opts.help) { print_usage(); return 0; }

    clew::single_instance_guard instance{CLEW_MUTEX_NAME};
    if (instance.already_running()) {
        instance.activate_existing(CLEW_WINDOW_CLASS);
        return 0;
    }

#ifndef NDEBUG
    clew::debug_console_session console;
#endif
    if (default_gui_mode) opts.gui_mode = true;

    setup_logger();
    PC_LOG_INFO("=== Clew {} (three-layer refactor) ===", CLEW_VERSION);

    if (!is_elevated()) {
        PC_LOG_ERROR("Clew requires administrator privileges");
        return 1;
    }
    PC_LOG_INFO("Running with administrator privileges");

    clew::winsock_session winsock;
    if (!winsock.ok()) {
        PC_LOG_ERROR("Failed to initialize Winsock");
        return 1;
    }

    try {
        clew::app a{opts, hinstance};
        return a.run();
    } catch (const std::exception& e) {
        PC_LOG_ERROR("Startup failed: {}", e.what());
        return 1;
    }
}

#ifdef CLEW_HAS_WEBVIEW2
int WINAPI WinMain(HINSTANCE hinstance, HINSTANCE, LPSTR, int) {
    return run_app(__argc, __argv, hinstance, /*default_gui=*/true);
}
#else
int main(int argc, char* argv[]) {
    return run_app(argc, argv, GetModuleHandle(nullptr), /*default_gui=*/false);
}
#endif
