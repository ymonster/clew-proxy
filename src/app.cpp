#include "app.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>
#include <utility>

#include "common/api_exception.hpp"
#include "config/config_change_tag.hpp"
#include "config/types.hpp"
#include "core/log.hpp"

namespace clew {

namespace {

bool pump_pending_messages() {
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

} // namespace

app::app(const cli_options& opts, HINSTANCE hinstance)
    : opts_(opts)
    , hinstance_(hinstance)
    , strand_(asio::make_strand(ioc_))
    , cfg_store_(config_)
    , tree_mgr_(ioc_, strand_)
    , exec_(tree_mgr_, strand_)
    , dns_mgr_(ioc_)
    , acceptor_(ioc_, *port_tracker_, strand_, tcp_groups_)
    , socks5_udp_mgr_(ioc_, udp_groups_)
    , projection_(tree_mgr_, strand_)
    , cfg_bridge_(cfg_store_)
    , config_svc_(cfg_store_)
    , connection_svc_(exec_, udp_port_tracker_.get())
    , group_svc_(exec_, cfg_store_)
    , icon_svc_(icons_)
    , process_svc_(exec_)
    , rule_svc_(exec_, cfg_store_)
    , stats_svc_(exec_)
    , ctx_{
        .config      = config_svc_,
        .connections = connection_svc_,
        .groups      = group_svc_,
        .icons       = icon_svc_,
        .processes   = process_svc_,
        .rules       = rule_svc_,
        .stats       = stats_svc_,
      }
    , api_server_(API_PORT, ctx_, opts_.static_dir)
{
    config_.load();
    set_log_level(config_.get_v2().log_level);

    num_workers_ = config_.get_v2().io_threads;
    if (num_workers_ <= 0) {
        num_workers_ = std::max(2u, std::thread::hardware_concurrency() / 2);
    }
    PC_LOG_INFO("io_context with {} worker threads", num_workers_);

    tree_mgr_.rules().set_auto_rules(config_.get_v2().auto_rules);
    PC_LOG_INFO("Loaded {} auto rules", config_.get_v2().auto_rules.size());

    sync_groups();
    dns_mgr_.recover_crash_state();

    redirect_port_ = acceptor_.start();
    PC_LOG_INFO("Acceptor listening on port {}", redirect_port_);

    wd_socket_      = std::make_unique<windivert_socket>(ioc_, strand_, tree_mgr_.tree(), *port_tracker_);
    wd_network_     = std::make_unique<windivert_network>(*port_tracker_, redirect_port_);
    wd_socket_udp_  = std::make_unique<windivert_socket_udp>(ioc_, strand_, tree_mgr_.tree(),
                                                              tree_mgr_.rules(), *udp_port_tracker_);
    wd_network_udp_ = std::make_unique<windivert_network_udp>(*udp_port_tracker_, UDP_RELAY_PORT,
                                                               udp_session_table_);

    icons_.init();

    tree_mgr_.add_listener(&projection_);

    wire_observers();

    gui_ = create_gui();

    // Wire backend->frontend push channel. The WebView2 host implements the
    // sink; setting it on projection / cfg_bridge enables broadcast. The
    // ready-handshake callback runs on the UI thread; replay_to_frontend
    // reads the atomic snapshot (no strand needed) and pushes back through
    // the same WM_PUSH_TO_FRONTEND marshalling path.
    if (gui_) {
        projection_.set_sink(gui_.get());
        cfg_bridge_.set_sink(gui_.get());
        gui_->set_on_ready([this]() {
            projection_.replay_to_frontend();
        });
    }
}

app::~app() noexcept {
    shutdown();
}

int app::run() {
    start_servers_and_workers();
    wait_for_tree_init();
    start_traffic_layers();
    PC_LOG_INFO("Clew is ready.");
    int rc = run_main_loop();
    shutdown();
    return rc;
}

void app::sync_groups() {
    tcp_groups_.clear();
    udp_groups_.clear();
    for (const auto& g : config_.get_v2().proxy_groups) {
        tcp_groups_[g.id] = {g.host, g.port};
        udp_groups_[g.id] = {g.host, g.port};
    }
    PC_LOG_INFO("Proxy groups synced: {} groups", tcp_groups_.size());
}

std::pair<std::string, uint16_t> app::pick_proxy_endpoint() const {
    const auto& v2 = config_.get_v2();
    if (!v2.proxy_groups.empty()) {
        return {v2.proxy_groups[0].host, v2.proxy_groups[0].port};
    }
    return {v2.default_proxy.host, v2.default_proxy.port};
}

void app::wire_observers() {
    cfg_store_.subscribe(
        [this](const ConfigV2& cfg, config_change /*tag*/) {
            try {
                exec_.command([rules = cfg.auto_rules](domain::process_tree_manager& m) {
                    m.apply_auto_rules_from_config(rules);
                });
            } catch (const api_exception& e) {
                PC_LOG_WARN("[config-sync] rule reload failed: {}", e.message());
            }
            sync_groups();
            set_log_level(cfg.log_level);
            auto [ph, pp] = pick_proxy_endpoint();
            dns_mgr_.apply(cfg.dns, ph, pp);
        });
}

std::unique_ptr<webview_app> app::create_gui() {
    if (!opts_.gui_mode) return nullptr;

    PC_LOG_INFO("Launching WebView2 GUI...");
    if (!is_webview2_available()) {
        PC_LOG_WARN("WebView2 not available, falling back to console mode");
        return nullptr;
    }

    const auto&  ui_cfg = config_.get_v2().ui;
    // CLEW_DEV_URL lets a developer point the embedded WebView2 at a Vite
    // dev server (e.g. http://localhost:5173) for HMR-driven frontend work.
    // Production builds always navigate to the embedded static files served
    // by the local HTTP API. WebView2 still provides PostWebMessageAsJson
    // regardless of which page is loaded.
    std::wstring url;
    if (auto* dev = std::getenv("CLEW_DEV_URL"); dev && *dev) {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, dev, -1, nullptr, 0);
        if (wlen > 0) {
            url.resize(static_cast<size_t>(wlen - 1));
            MultiByteToWideChar(CP_UTF8, 0, dev, -1, url.data(), wlen);
            PC_LOG_INFO("CLEW_DEV_URL set; loading dev frontend from {}", dev);
        }
    }
    if (url.empty()) {
        url = std::format(L"http://127.0.0.1:{}/", API_PORT);
    }
    auto gui = std::make_unique<webview_app>(url, ui_cfg.window_width, ui_cfg.window_height);
    gui->set_title(L"Clew");
    gui->set_initial_rect(ui_cfg.window_x, ui_cfg.window_y,
                           ui_cfg.window_width, ui_cfg.window_height);
    gui->set_close_to_tray(ui_cfg.close_to_tray);
    gui->set_devtools_enabled(opts_.devtools);
    gui->set_on_move_resize([this](int x, int y, int w, int h) {
        auto& ui      = config_.get_v2().ui;
        ui.window_x   = x;
        ui.window_y   = y;
        ui.window_width  = w;
        ui.window_height = h;
        config_.save();
    });

    if (!gui->create(hinstance_)) {
        PC_LOG_ERROR("WebView2 window creation failed");
        return nullptr;
    }
    return gui;
}

void app::start_servers_and_workers() {
    if (!api_server_.start()) {
        throw startup_error{"Failed to start HTTP API server"};
    }
    PC_LOG_INFO("HTTP API: http://127.0.0.1:{}/", API_PORT);

    auto [ph, pp] = pick_proxy_endpoint();
    dns_mgr_.apply(config_.get_v2().dns, ph, pp);

    tree_mgr_.start();

    workers_.reserve(static_cast<size_t>(num_workers_));
    for (int i = 0; i < num_workers_; ++i) {
        workers_.emplace_back([this]() { ioc_.run(); });
    }
}

void app::wait_for_tree_init() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!tree_mgr_.is_initialized() && !init_interrupted_ &&
           std::chrono::steady_clock::now() < deadline) {
        if (gui_ && !pump_pending_messages()) {
            init_interrupted_ = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!tree_mgr_.is_initialized() && !init_interrupted_) {
        PC_LOG_ERROR("Tree initialization timeout");
    }
}

void app::start_traffic_layers() {
    if (!tree_mgr_.is_initialized()) return;

    // Initial auto-rule scan + notify. apply_auto_rules_from_config re-applies
    // the same list already loaded above, but routes through manager's
    // notify_tree_changed() so projection refreshes its snapshot.
    asio::post(strand_, [this]() {
        auto rules_copy = tree_mgr_.rules().auto_rules();
        tree_mgr_.apply_auto_rules_from_config(rules_copy);
        PC_LOG_INFO("Initial auto-rule scan complete");
    });

    if (wd_socket_->open()) {
        wd_socket_->start();
        PC_LOG_INFO("WinDivert SOCKET layer started");
    } else {
        PC_LOG_ERROR("WinDivert SOCKET layer failed — traffic interception disabled");
    }

    if (wd_network_->open()) {
        wd_network_->start(2);
        PC_LOG_INFO("WinDivert NETWORK layer started ({} workers)", 2);
    } else {
        PC_LOG_ERROR("WinDivert NETWORK layer failed — traffic interception disabled");
    }

    if (wd_socket_udp_->open()) {
        wd_socket_udp_->start();
        PC_LOG_INFO("WinDivert UDP SOCKET layer started");
    }

    if (wd_network_udp_->open()) {
        wd_network_udp_->start(2);
        PC_LOG_INFO("WinDivert UDP NETWORK layer started");

        udp_relay_ = std::make_unique<UdpRelay>(
            ioc_, udp_session_table_, socks5_udp_mgr_,
            wd_network_udp_->handle(), UDP_RELAY_PORT);
        if (udp_relay_->start()) {
            PC_LOG_INFO("UDP Relay started on port {}", UDP_RELAY_PORT);
        }
    }
}

int app::run_main_loop() {
    if (gui_) {
        gui_->run();
    } else {
        PC_LOG_ERROR("GUI creation failed — this build requires WebView2.");
        std::this_thread::sleep_for(std::chrono::hours(1));
    }
    return 0;
}

void app::shutdown() noexcept {
    if (shut_down_.exchange(true)) return;
    PC_LOG_INFO("Shutting down...");

    // DNS first — touches system-wide state visible to the user.
    dns_mgr_.stop();

    socks5_udp_mgr_.stop();
    if (udp_relay_)      udp_relay_->stop();
    if (wd_network_udp_) wd_network_udp_->close();
    if (wd_socket_udp_)  wd_socket_udp_->close();
    PC_LOG_INFO("UDP layers closed");

    if (wd_socket_)  wd_socket_->close();
    if (wd_network_) wd_network_->close();
    PC_LOG_INFO("WinDivert layers closed");

    acceptor_.stop();
    api_server_.stop();

    ioc_.stop();
    for (auto& w : workers_) w.join();
    PC_LOG_INFO("io_context stopped, {} workers joined", num_workers_);

    tree_mgr_.stop();

    icons_.shutdown();
    quill::Backend::stop();
    PC_LOG_INFO("Goodbye!");
}

} // namespace clew
