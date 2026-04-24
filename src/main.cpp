// Clew — Process-level Traffic Hijacker
// main.cpp: v3 architecture — Asio + ETW + Flat Tree + WinDivert dual-layer

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <format>
#include <span>
#include <thread>
#include "core/log.hpp"
#include "core/version.hpp"
#include "core/scoped_exit.hpp"

#define ASIO_STANDALONE
#include <asio.hpp>

#include "config/types.hpp"
#include "config/config_manager.hpp"
#include "process/process_tree_manager.hpp"
#include "core/port_tracker.hpp"
#include "core/windivert_socket.hpp"
#include "core/windivert_network.hpp"
#include "proxy/acceptor.hpp"
#include "proxy/relay.hpp"
#include "udp/udp_port_tracker.hpp"
#include "udp/windivert_socket_udp.hpp"
#include "udp/windivert_network_udp.hpp"
#include "udp/udp_session_table.hpp"
#include "udp/socks5_udp_manager.hpp"
#include "udp/udp_relay.hpp"
#include "api/http_api_server.hpp"
#include "core/dns_forwarder.hpp"
#include "core/dns_manager.hpp"
#include "ui/webview_app.hpp"

// Drain all pending Win32 messages. Returns false if WM_QUIT was observed
// (caller should then stop its wait loop).
static bool pump_pending_messages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return true;
}

static bool is_elevated() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return false;
    clew::unique_handle token(raw_token);

    TOKEN_ELEVATION elevation;
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size))
        return false;
    return elevation.TokenIsElevated == TRUE;
}

struct cli_options {
    bool gui_mode = false;
    bool help = false;
    std::string static_dir;
};

static void print_usage() {
    std::cout << "Clew " CLEW_VERSION " - Process-level Traffic Hijacker\n\n";
    std::cout << "Usage: clew [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --gui           Launch with WebView2 GUI\n";
    std::cout << "  --static-dir    Path to static files directory\n";
    std::cout << "  --help, -h      Show this help message\n\n";
}

static cli_options parse_args(int argc, char* argv[]) {
    cli_options opts;
    std::span args(argv, argc);
    auto it = args.begin() + 1;  // skip program name
    while (it != args.end()) {
        std::string_view arg = *it++;
        if (arg == "--gui") opts.gui_mode = true;
        else if (arg == "--static-dir" && it != args.end()) opts.static_dir = *it++;
        else if (arg == "--help" || arg == "-h") opts.help = true;
    }
    return opts;
}

static constexpr const wchar_t* CLEW_MUTEX_NAME = L"Global\\Clew_SingleInstance";
static constexpr const wchar_t* CLEW_WINDOW_CLASS = L"ClewWebViewClass";

#ifdef CLEW_HAS_WEBVIEW2
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int) {
    int argc = __argc;
    char** argv = __argv;
    (void)lpCmdLine;
    cli_options opts = parse_args(argc, argv);

    clew::unique_handle mutex(CreateMutexW(nullptr, TRUE, CLEW_MUTEX_NAME));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(CLEW_WINDOW_CLASS, nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;  // mutex closed by unique_handle dtor
    }

#ifndef NDEBUG
    AllocConsole();
    FILE* fp_out = nullptr;
    FILE* fp_err = nullptr;
    freopen_s(&fp_out, "CONOUT$", "w", stdout);
    freopen_s(&fp_err, "CONOUT$", "w", stderr);
#endif
    opts.gui_mode = true;
#else
int main(int argc, char* argv[]) {
    cli_options opts = parse_args(argc, argv);
    HINSTANCE hInstance = GetModuleHandle(nullptr);
    clew::unique_handle mutex;
#endif

    if (opts.help) { print_usage(); return 0; }

    // ============================================================
    // Logging (quill)
    // ============================================================
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

#ifndef NDEBUG
    auto console_sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("console");
    clew::g_logger = quill::Frontend::create_or_get_logger(
        "clew", {std::move(file_sink), std::move(console_sink)},
        quill::PatternFormatterOptions{
            "%(time) [%(log_level_short_code)] %(message)",
            "%Y-%m-%d %H:%M:%S.%Qus"});
#else
    clew::g_logger = quill::Frontend::create_or_get_logger(
        "clew", std::move(file_sink),
        quill::PatternFormatterOptions{
            "%(time) [%(log_level_short_code)] %(message)",
            "%Y-%m-%d %H:%M:%S.%Qus"});
#endif

    PC_LOG_INFO("=== Clew {} (v3 architecture) ===", CLEW_VERSION);

    if (!is_elevated()) {
        PC_LOG_ERROR("Clew requires administrator privileges");
        return 1;
    }
    PC_LOG_INFO("Running with administrator privileges");

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        PC_LOG_ERROR("Failed to initialize Winsock");
        return 1;
    }

    // ============================================================
    // 1. Config
    // ============================================================
    clew::config_manager config;
    config.load();
    clew::set_log_level(config.get_v2().log_level);

    // ============================================================
    // 2. Asio io_context + worker threads
    // ============================================================
    asio::io_context ioc;
    auto strand = asio::make_strand(ioc);

    int num_workers = config.get_v2().io_threads;
    if (num_workers <= 0)
        num_workers = std::max(2u, std::thread::hardware_concurrency() / 2);
    PC_LOG_INFO("io_context with {} worker threads", num_workers);

    // Startup-phase interrupt flag — only gates the init-wait loop below,
    // set to true if the user closes the window before process tree init
    // finishes. After gui->run() starts, the Win32 message loop drives
    // shutdown on its own (WM_DESTROY → PostQuitMessage → gui->run() returns).
    bool init_interrupted = false;

    // ============================================================
    // 3. Process Tree Manager (ETW + NtQuery + Flat Tree + Rules)
    // ============================================================
    clew::process_tree_manager tree_mgr(ioc);

    // Load auto rules
    tree_mgr.rules().set_auto_rules(config.get_v2().auto_rules);
    PC_LOG_INFO("Loaded {} auto rules", config.get_v2().auto_rules.size());

    // ============================================================
    // 4. PortTracker (65536 array, ~4MB heap)
    // ============================================================
    auto port_tracker = std::make_unique<clew::PortTracker>();

    // ============================================================
    // 5. Proxy groups map (for relay coroutines)
    // ============================================================
    std::unordered_map<uint32_t, clew::ProxyGroupConfig> groups;
    auto sync_groups = [&]() {
        groups.clear();
        for (const auto& g : config.get_v2().proxy_groups) {
            groups[g.id] = {g.host, g.port};
        }
        PC_LOG_INFO("Proxy groups synced: {} groups", groups.size());
    };
    sync_groups();

    // Pick first proxy group as DNS upstream hop (SOCKS5 endpoint)
    auto pick_proxy_endpoint = [&]() -> std::pair<std::string, uint16_t> {
        const auto& v2 = config.get_v2();
        if (!v2.proxy_groups.empty()) {
            return {v2.proxy_groups[0].host, v2.proxy_groups[0].port};
        }
        return {v2.default_proxy.host, v2.default_proxy.port};
    };
    clew::DnsManager dns_mgr(ioc);
    dns_mgr.recover_crash_state();

    // ============================================================
    // 6. Acceptor (Asio TCP listener for redirected connections)
    // ============================================================
    clew::async_acceptor acceptor(ioc, *port_tracker, strand, groups);
    uint16_t redirect_port = acceptor.start();
    PC_LOG_INFO("Acceptor listening on port {}", redirect_port);

    // ============================================================
    // 7. WinDivert layers
    // ============================================================
    clew::windivert_socket wd_socket(ioc, strand, tree_mgr.tree(), *port_tracker);
    clew::windivert_network wd_network(*port_tracker, redirect_port);

    // ============================================================
    // 7b. UDP components (independent from TCP)
    // ============================================================
    constexpr uint16_t UDP_RELAY_PORT = 19999;

    auto udp_port_tracker = std::make_unique<clew::UdpPortTracker>();

    // UDP proxy groups (same hosts as TCP, separate map for UdpProxyGroupConfig)
    std::unordered_map<uint32_t, clew::UdpProxyGroupConfig> udp_groups;
    auto sync_udp_groups = [&]() {
        udp_groups.clear();
        for (const auto& g : config.get_v2().proxy_groups) {
            udp_groups[g.id] = {g.host, g.port};
        }
    };
    sync_udp_groups();

    clew::UdpSessionTable udp_session_table;

    clew::windivert_socket_udp wd_socket_udp(
        ioc, strand, tree_mgr.tree(), tree_mgr.rules(), *udp_port_tracker);

    clew::windivert_network_udp wd_network_udp(
        *udp_port_tracker, UDP_RELAY_PORT, udp_session_table);

    clew::Socks5UdpManager socks5_udp_mgr(ioc, udp_groups);

    // UdpRelay created after wd_network_udp.open() so handle is valid
    std::unique_ptr<clew::UdpRelay> udp_relay;

    // ============================================================
    // 8. HTTP API Server
    // ============================================================
    constexpr int API_PORT = 18080;
    clew::http_api_server api_server(API_PORT, tree_mgr, config, strand, opts.static_dir);
    api_server.set_port_tracker(port_tracker.get());
    api_server.set_udp_port_tracker(udp_port_tracker.get());

    // Icon cache (GDI+ based, provides /api/icon endpoint)
    clew::icon_cache icons;
    if (icons.init()) {
        api_server.set_icon_cache(&icons);
    }


    // Wire tree change → API snapshot update + SSE push
    tree_mgr.set_on_tree_changed([&api_server]() {
        api_server.on_tree_changed();
    });

    // Config change callback (from Monaco editor save)
    api_server.set_on_config_change(
        [&strand, &tree_mgr, &sync_groups, &sync_udp_groups, &pick_proxy_endpoint, &dns_mgr]
        (const clew::ConfigV2& cfg) {
        // Reload auto rules on strand
        asio::post(strand, [&tree_mgr, rules = cfg.auto_rules]() {
            tree_mgr.rules().set_auto_rules(rules);
            tree_mgr.rules().apply_auto_rules(tree_mgr.tree());
        });
        sync_groups();
        sync_udp_groups();
        clew::set_log_level(cfg.log_level);

        // DNS hot-reload
        auto [ph, pp] = pick_proxy_endpoint();
        dns_mgr.apply(cfg.dns, ph, pp);

#ifdef CLEW_HAS_WEBVIEW2
        // Will be wired to webview_app below if GUI mode
#endif
    });

    // ============================================================
    // Start services
    // ============================================================

    // Start API server
    if (!api_server.start()) {
        PC_LOG_ERROR("Failed to start API server");
        WSACleanup();
        return 1;
    }
    PC_LOG_INFO("HTTP API: http://127.0.0.1:{}/", API_PORT);

    // Apply DNS config (forwarder start + system DNS configuration)
    {
        auto [ph, pp] = pick_proxy_endpoint();
        dns_mgr.apply(config.get_v2().dns, ph, pp);
    }

    // Start tree manager (ETW → NtQuery → build tree → ready)
    tree_mgr.start();

    // Start io_context workers (processes tree_mgr initialization + acceptor + SOCKET events)
    std::vector<std::jthread> workers;
    for (int i = 0; i < num_workers; i++) {
        workers.emplace_back([&ioc]() { ioc.run(); });
    }

    // ============================================================
    // GUI: Create window early so WebView2 init runs in parallel
    // ============================================================
    std::unique_ptr<clew::webview_app> gui;
    bool gui_created = false;
    if (opts.gui_mode) {
        PC_LOG_INFO("Launching WebView2 GUI...");
        if (clew::is_webview2_available()) {
            const auto& ui_cfg = config.get_v2().ui;
            std::wstring url = std::format(L"http://127.0.0.1:{}/", API_PORT);
            gui = std::make_unique<clew::webview_app>(url, ui_cfg.window_width, ui_cfg.window_height);
            gui->set_title(L"Clew");
            gui->set_initial_rect(ui_cfg.window_x, ui_cfg.window_y, ui_cfg.window_width, ui_cfg.window_height);
            gui->set_close_to_tray(ui_cfg.close_to_tray);
#ifdef NDEBUG
            gui->set_devtools_enabled(false);
#endif
            // No on_close handler needed: the Win32 message loop exits on
            // WM_DESTROY (PostQuitMessage), which returns from gui->run()
            // naturally — same path for X-button close and tray Exit.
            gui->set_on_move_resize([&config](int x, int y, int w, int h) {
                auto& ui = config.get_v2().ui;
                ui.window_x = x; ui.window_y = y;
                ui.window_width = w; ui.window_height = h;
                config.save();
            });
            gui_created = gui->create(hInstance);
            if (!gui_created) {
                PC_LOG_ERROR("WebView2 window creation failed");
                gui.reset();
            }
        } else {
            PC_LOG_WARN("WebView2 not available, falling back to console mode");
        }
    }

    // Wait for tree initialization before opening WinDivert
    // Pump Win32 messages during wait so WebView2 can load in parallel
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!tree_mgr.is_initialized() && !init_interrupted &&
               std::chrono::steady_clock::now() < deadline) {
            if (gui_created && !pump_pending_messages()) {
                init_interrupted = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!tree_mgr.is_initialized() && !init_interrupted)
            PC_LOG_ERROR("Tree initialization timeout");
    }

    if (tree_mgr.is_initialized()) {
        // Apply auto rules to initial tree
        asio::post(strand, [&tree_mgr]() {
            tree_mgr.rules().apply_auto_rules(tree_mgr.tree());
            PC_LOG_INFO("Initial auto-rule scan complete");
        });

        // Open WinDivert layers
        if (wd_socket.open()) {
            wd_socket.start();
            PC_LOG_INFO("WinDivert SOCKET layer started");
        } else {
            PC_LOG_ERROR("WinDivert SOCKET layer failed — traffic interception disabled");
        }

        if (wd_network.open()) {
            wd_network.start(2);
            PC_LOG_INFO("WinDivert NETWORK layer started ({} workers)", 2);
        } else {
            PC_LOG_ERROR("WinDivert NETWORK layer failed — traffic interception disabled");
        }

        // UDP layers
        if (wd_socket_udp.open()) {
            wd_socket_udp.start();
            PC_LOG_INFO("WinDivert UDP SOCKET layer started");
        }

        if (wd_network_udp.open()) {
            wd_network_udp.start(2);
            PC_LOG_INFO("WinDivert UDP NETWORK layer started");

            // Create relay now that handle is valid
            udp_relay = std::make_unique<clew::UdpRelay>(
                ioc, udp_session_table, socks5_udp_mgr,
                wd_network_udp.handle(), UDP_RELAY_PORT);
            if (udp_relay->start()) {
                PC_LOG_INFO("UDP Relay started on port {}", UDP_RELAY_PORT);
            }

            PC_LOG_INFO("UDP relay started (per-port sessions, no warmup needed)");
        }
    }

    PC_LOG_INFO("Clew is ready.");

    // ============================================================
    // Main loop — Win32 message pump. Exits when the user closes the
    // window (X button) or picks Exit from the tray menu; both paths
    // end in WM_DESTROY → PostQuitMessage → gui->run() returns.
    // ============================================================
    if (gui_created) {
        gui->run();
    } else {
        // GUI creation failed (WebView2 missing / init error). The CLI
        // fallback is not a supported product mode; park the thread so
        // background engines can finish any in-flight work, then rely
        // on the user terminating via Task Manager.
        PC_LOG_ERROR("GUI creation failed — this build requires WebView2.");
        std::this_thread::sleep_for(std::chrono::hours(1));
    }

    // ============================================================
    // Graceful shutdown
    // ============================================================
    PC_LOG_INFO("Shutting down...");

    // 1a. Stop UDP components first (reverse startup order)
    dns_mgr.stop();  // stops forwarder + restores system DNS
    socks5_udp_mgr.stop();
    if (udp_relay) udp_relay->stop();
    wd_network_udp.close();
    wd_socket_udp.close();
    PC_LOG_INFO("UDP layers closed");

    // 1b. Stop TCP WinDivert (no new intercepts)
    wd_socket.close();
    wd_network.close();
    PC_LOG_INFO("WinDivert layers closed");

    // 2. Stop acceptor (no new relay coroutines)
    acceptor.stop();

    // 3. Stop API server first (breaks SSE connections, stops httplib)
    api_server.stop();

    // 4. Stop io_context (relay coroutines will be cancelled)
    ioc.stop();
    for (auto& w : workers) w.join();
    PC_LOG_INFO("io_context stopped, {} workers joined", num_workers);

    // 5. Stop ETW + tree manager
    tree_mgr.stop();

    // 6. Cleanup (mutex closed automatically by unique_handle dtor on scope exit)
    icons.shutdown();
    WSACleanup();
    quill::Backend::stop();
    PC_LOG_INFO("Goodbye!");

    return 0;
}
