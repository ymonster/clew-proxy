// Clew — Process-level Traffic Hijacker
// main.cpp: three-layer architecture wiring
//   domain (strand_bound + process_tree_manager + config_store)
//     ^
//     |
//   application services (9) + auth_middleware
//     ^
//     |
//   transport (sse_hub + projection + bridge + http_api_server)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.hpp"
#include "core/scoped_exit.hpp"
#include "core/version.hpp"

#define ASIO_STANDALONE
#include <asio.hpp>

// Config + domain
#include "config/config_manager.hpp"
#include "config/config_store.hpp"
#include "config/types.hpp"
#include "domain/process_tree_manager.hpp"
#include "domain/strand_bound.hpp"

// Transport + projection + bridge + SSE
#include "common/api_context.hpp"
#include "projection/config_sse_bridge.hpp"
#include "projection/process_projection.hpp"
#include "transport/http_api_server.hpp"
#include "transport/sse_hub.hpp"

// Application services + middleware
#include "auth/auth_middleware.hpp"
#include "services/auth_service.hpp"
#include "services/config_service.hpp"
#include "services/connection_service.hpp"
#include "services/group_service.hpp"
#include "services/icon_service.hpp"
#include "services/process_tree_service.hpp"
#include "services/rule_service.hpp"
#include "services/shell_service.hpp"
#include "services/stats_service.hpp"

// Hot-path components (reused verbatim)
#include "api/icon_cache.hpp"
#include "core/dns_forwarder.hpp"
#include "core/dns_manager.hpp"
#include "core/port_tracker.hpp"
#include "core/windivert_network.hpp"
#include "core/windivert_socket.hpp"
#include "proxy/acceptor.hpp"
#include "proxy/relay.hpp"
#include "udp/socks5_udp_manager.hpp"
#include "udp/udp_port_tracker.hpp"
#include "udp/udp_relay.hpp"
#include "udp/udp_session_table.hpp"
#include "udp/windivert_network_udp.hpp"
#include "udp/windivert_socket_udp.hpp"
#include "ui/webview_app.hpp"

// ---------------------------------------------------------------------------

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
    auto it = args.begin() + 1;
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
        return 0;
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

    PC_LOG_INFO("=== Clew {} (three-layer refactor) ===", CLEW_VERSION);

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
    if (num_workers <= 0) {
        num_workers = std::max(2u, std::thread::hardware_concurrency() / 2);
    }
    PC_LOG_INFO("io_context with {} worker threads", num_workers);

    bool init_interrupted = false;

    // ============================================================
    // 3. Domain layer: config_store + process_tree_manager + strand_bound
    // ============================================================
    clew::config_store cfg_store(config);
    clew::domain::process_tree_manager tree_mgr(ioc, strand);
    clew::strand_bound_manager exec(tree_mgr, strand);

    // Preload auto rules into rule_engine before ETW starts (safe — no
    // concurrent access yet). The config_store observer registered below
    // handles subsequent hot-reload.
    tree_mgr.rules().set_auto_rules(config.get_v2().auto_rules);
    PC_LOG_INFO("Loaded {} auto rules", config.get_v2().auto_rules.size());

    // ============================================================
    // 4. Hot-path infrastructure (unchanged from legacy)
    // ============================================================
    auto port_tracker = std::make_unique<clew::PortTracker>();

    std::unordered_map<uint32_t, clew::ProxyGroupConfig> tcp_groups;
    std::unordered_map<uint32_t, clew::UdpProxyGroupConfig> udp_groups;

    auto sync_groups = [&]() {
        tcp_groups.clear();
        udp_groups.clear();
        for (const auto& g : config.get_v2().proxy_groups) {
            tcp_groups[g.id] = {g.host, g.port};
            udp_groups[g.id] = {g.host, g.port};
        }
        PC_LOG_INFO("Proxy groups synced: {} groups", tcp_groups.size());
    };
    sync_groups();

    auto pick_proxy_endpoint = [&]() -> std::pair<std::string, uint16_t> {
        const auto& v2 = config.get_v2();
        if (!v2.proxy_groups.empty()) {
            return {v2.proxy_groups[0].host, v2.proxy_groups[0].port};
        }
        return {v2.default_proxy.host, v2.default_proxy.port};
    };

    clew::DnsManager dns_mgr(ioc);
    dns_mgr.recover_crash_state();

    clew::async_acceptor acceptor(ioc, *port_tracker, strand, tcp_groups);
    uint16_t redirect_port = acceptor.start();
    PC_LOG_INFO("Acceptor listening on port {}", redirect_port);

    clew::windivert_socket  wd_socket(ioc, strand, tree_mgr.tree(), *port_tracker);
    clew::windivert_network wd_network(*port_tracker, redirect_port);

    constexpr uint16_t UDP_RELAY_PORT = 19999;

    auto udp_port_tracker = std::make_unique<clew::UdpPortTracker>();
    clew::UdpSessionTable udp_session_table;
    clew::windivert_socket_udp  wd_socket_udp(ioc, strand, tree_mgr.tree(),
                                               tree_mgr.rules(), *udp_port_tracker);
    clew::windivert_network_udp wd_network_udp(*udp_port_tracker, UDP_RELAY_PORT,
                                                udp_session_table);
    clew::Socks5UdpManager socks5_udp_mgr(ioc, udp_groups);
    std::unique_ptr<clew::UdpRelay> udp_relay;

    clew::icon_cache icons;
    icons.init();

    // ============================================================
    // 5. Transport wiring: sse_hub -> process_projection + config_sse_bridge
    // ============================================================
    clew::sse_hub          sse;
    clew::process_projection  projection(tree_mgr, sse);
    clew::config_sse_bridge   cfg_bridge(cfg_store, sse);
    tree_mgr.add_listener(&projection);

    // ============================================================
    // 6. Application services (9)
    // ============================================================
    clew::auth_service         auth_svc(cfg_store);
    clew::config_service       config_svc(cfg_store);
    clew::connection_service   connection_svc(exec, udp_port_tracker.get());
    clew::group_service        group_svc(exec, cfg_store);
    clew::icon_service         icon_svc(icons);
    clew::process_tree_service process_svc(exec);
    process_svc.set_snapshot_getter([&projection]() { return projection.tree_snapshot(); });
    clew::rule_service         rule_svc(exec, cfg_store);
    clew::shell_service        shell_svc;
    clew::stats_service        stats_svc(exec);

    clew::auth_middleware auth_mw(cfg_store);

    clew::api_context ctx{
        .auth        = auth_svc,
        .config      = config_svc,
        .connections = connection_svc,
        .groups      = group_svc,
        .icons       = icon_svc,
        .processes   = process_svc,
        .rules       = rule_svc,
        .shell       = shell_svc,
        .stats       = stats_svc,
        .sse         = sse,
    };

    // ============================================================
    // 7. Config observers (side-effect fanout)
    //    - rule engine sync: re-apply auto rules on strand
    //    - proxy groups sync: update acceptor + udp manager maps
    //    - dns hot-reload
    //    - log level
    //
    //    (config_sse_bridge already subscribes separately in its ctor.)
    // ============================================================
    cfg_store.subscribe(
        [&exec, &sync_groups, &dns_mgr, &pick_proxy_endpoint]
        (const clew::ConfigV2& cfg, clew::config_change /*tag*/) {
            try {
                exec.command([rules = cfg.auto_rules](clew::domain::process_tree_manager& m) {
                    m.apply_auto_rules_from_config(rules);
                });
            } catch (const std::exception& e) {
                PC_LOG_WARN("[config-sync] rule reload failed: {}", e.what());
            }

            sync_groups();
            clew::set_log_level(cfg.log_level);

            auto [ph, pp] = pick_proxy_endpoint();
            dns_mgr.apply(cfg.dns, ph, pp);
        });

    // ============================================================
    // 8. HTTP API server (transport layer)
    // ============================================================
    constexpr int API_PORT = 18080;
    clew::transport::http_api_server api_server(API_PORT, ctx, auth_mw, opts.static_dir);

    // ============================================================
    // Start services
    // ============================================================
    if (!api_server.start()) {
        PC_LOG_ERROR("Failed to start HTTP API server");
        WSACleanup();
        return 1;
    }
    PC_LOG_INFO("HTTP API: http://127.0.0.1:{}/", API_PORT);

    {
        auto [ph, pp] = pick_proxy_endpoint();
        dns_mgr.apply(config.get_v2().dns, ph, pp);
    }

    tree_mgr.start();

    std::vector<std::jthread> workers;
    for (int i = 0; i < num_workers; ++i) {
        workers.emplace_back([&ioc]() { ioc.run(); });
    }

    // ============================================================
    // GUI (WebView2)
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
            gui->set_initial_rect(ui_cfg.window_x, ui_cfg.window_y,
                                   ui_cfg.window_width, ui_cfg.window_height);
            gui->set_close_to_tray(ui_cfg.close_to_tray);
#ifdef NDEBUG
            gui->set_devtools_enabled(false);
#endif
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
        if (!tree_mgr.is_initialized() && !init_interrupted) {
            PC_LOG_ERROR("Tree initialization timeout");
        }
    }

    if (tree_mgr.is_initialized()) {
        // Initial auto-rule scan + notify. apply_auto_rules_from_config
        // re-applies the same list already loaded above, but routes through
        // manager.notify_tree_changed() so projection refreshes its snapshot.
        asio::post(strand, [&tree_mgr]() {
            auto rules_copy = tree_mgr.rules().auto_rules();
            tree_mgr.apply_auto_rules_from_config(rules_copy);
            PC_LOG_INFO("Initial auto-rule scan complete");
        });

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

        if (wd_socket_udp.open()) {
            wd_socket_udp.start();
            PC_LOG_INFO("WinDivert UDP SOCKET layer started");
        }

        if (wd_network_udp.open()) {
            wd_network_udp.start(2);
            PC_LOG_INFO("WinDivert UDP NETWORK layer started");

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
    // Main loop (WM message pump or idle park)
    // ============================================================
    if (gui_created) {
        gui->run();
    } else {
        PC_LOG_ERROR("GUI creation failed — this build requires WebView2.");
        std::this_thread::sleep_for(std::chrono::hours(1));
    }

    // ============================================================
    // Graceful shutdown (reverse order)
    // ============================================================
    PC_LOG_INFO("Shutting down...");

    dns_mgr.stop();
    socks5_udp_mgr.stop();
    if (udp_relay) udp_relay->stop();
    wd_network_udp.close();
    wd_socket_udp.close();
    PC_LOG_INFO("UDP layers closed");

    wd_socket.close();
    wd_network.close();
    PC_LOG_INFO("WinDivert layers closed");

    acceptor.stop();

    api_server.stop();

    ioc.stop();
    for (auto& w : workers) w.join();
    PC_LOG_INFO("io_context stopped, {} workers joined", num_workers);

    tree_mgr.stop();
    sse.stop();

    icons.shutdown();
    WSACleanup();
    quill::Backend::stop();
    PC_LOG_INFO("Goodbye!");

    return 0;
}
