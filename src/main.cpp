// Clew — Process-level Traffic Hijacker
// main.cpp: v3 architecture — Asio + ETW + Flat Tree + WinDivert dual-layer

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <iostream>
#include <csignal>
#include <vector>
#include <string>
#include <thread>
#include "core/log.hpp"
#include "core/version.hpp"

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

static std::atomic<bool> g_running{true};

static void signal_handler(int sig) {
    PC_LOG_INFO("Received signal {}, shutting down...", sig);
    g_running = false;
}

static bool is_elevated() {
    BOOL elevated = FALSE;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation;
        DWORD size;
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated;
        }
        CloseHandle(token);
    }
    return elevated == TRUE;
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
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--gui") opts.gui_mode = true;
        else if (arg == "--static-dir" && i + 1 < argc) opts.static_dir = argv[++i];
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

    HANDLE mutex = CreateMutexW(nullptr, TRUE, CLEW_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(CLEW_WINDOW_CLASS, nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

#ifndef NDEBUG
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
#endif
    opts.gui_mode = true;
#else
int main(int argc, char* argv[]) {
    cli_options opts = parse_args(argc, argv);
    HINSTANCE hInstance = GetModuleHandle(nullptr);
    HANDLE mutex = nullptr;
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
    tree_mgr.set_on_tree_changed([&]() {
        api_server.on_tree_changed();
    });

    // Config change callback (from Monaco editor save)
    api_server.set_on_config_change([&](const clew::ConfigV2& cfg) {
        // Reload auto rules on strand
        asio::post(strand, [&, rules = cfg.auto_rules]() {
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
    std::vector<std::thread> workers;
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
            std::wstring url = L"http://127.0.0.1:" + std::to_wstring(API_PORT) + L"/";
            gui = std::make_unique<clew::webview_app>(url, ui_cfg.window_width, ui_cfg.window_height);
            gui->set_title(L"Clew");
            gui->set_initial_rect(ui_cfg.window_x, ui_cfg.window_y, ui_cfg.window_width, ui_cfg.window_height);
            gui->set_close_to_tray(ui_cfg.close_to_tray);
#ifdef NDEBUG
            gui->set_devtools_enabled(false);
#endif
            gui->set_on_close([&]() { g_running = false; });
            gui->set_on_move_resize([&](int x, int y, int w, int h) {
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
        auto t0 = std::chrono::steady_clock::now();
        while (!tree_mgr.is_initialized() && g_running) {
            if (gui_created) {
                MSG msg;
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) {
                        PostQuitMessage(static_cast<int>(msg.wParam));
                        g_running = false;
                        break;
                    }
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
                if (!g_running) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(30)) {
                PC_LOG_ERROR("Tree initialization timeout");
                break;
            }
        }
    }

    if (tree_mgr.is_initialized()) {
        // Apply auto rules to initial tree
        asio::post(strand, [&]() {
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

        api_server.set_proxy_running(true);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    PC_LOG_INFO("Clew is ready.");

    // ============================================================
    // Main loop (GUI message pump or console wait)
    // ============================================================
    if (gui_created) {
        gui->run();
    } else {
        if (!opts.gui_mode)
            PC_LOG_INFO("Console mode. Press Ctrl+C to exit.");
        while (g_running) std::this_thread::sleep_for(std::chrono::seconds(1));
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

    // 6. Cleanup
    icons.shutdown();
    WSACleanup();
    quill::Backend::stop();
    if (mutex) CloseHandle(mutex);
    PC_LOG_INFO("Goodbye!");

    return 0;
}
