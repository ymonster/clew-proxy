#pragma once

// Single owner for clew's runtime object graph. main() constructs one,
// calls run(), and returns the exit code. The destructor calls shutdown()
// in explicit priority order (DNS first since it touches user-visible
// system state; quill backend last so shutdown messages still get through).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#define ASIO_STANDALONE
#include <asio.hpp>

#include "api/icon_cache.hpp"
#include "common/api_context.hpp"
#include "config/config_manager.hpp"
#include "config/config_store.hpp"
#include "core/dns_manager.hpp"
#include "core/port_tracker.hpp"
#include "core/windivert_network.hpp"
#include "core/windivert_socket.hpp"
#include "domain/process_tree_manager.hpp"
#include "domain/strand_bound.hpp"
#include "projection/config_sse_bridge.hpp"
#include "projection/process_projection.hpp"
#include "proxy/acceptor.hpp"
#include "services/config_service.hpp"
#include "services/connection_service.hpp"
#include "services/group_service.hpp"
#include "services/icon_service.hpp"
#include "services/process_tree_service.hpp"
#include "services/rule_service.hpp"
#include "services/stats_service.hpp"
#include "transport/http_api_server.hpp"
#include "udp/socks5_udp_manager.hpp"
#include "udp/udp_port_tracker.hpp"
#include "udp/udp_relay.hpp"
#include "udp/udp_session_table.hpp"
#include "udp/windivert_network_udp.hpp"
#include "udp/windivert_socket_udp.hpp"
#include "ui/webview_app.hpp"

namespace clew {

struct cli_options {
    bool        gui_mode        = false;
    bool        help            = false;
    bool        devtools        = false;  // --devtools: enable WebView2 DevTools (F12)
    bool        start_minimized = false;  // --minimized: hide main window on launch (used by autostart)
    std::string static_dir;
    std::string config_path;              // --config: explicit clew.json path; default = exe_directory()/clew.json
};

// Thrown by app's ctor / run() when a critical startup step fails
// (e.g. HTTP server can't bind). Caught by run_app() which logs + exits 1.
class startup_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class app {
public:
    app(const cli_options& opts, HINSTANCE hinstance);
    ~app() noexcept;

    app(const app&)            = delete;
    app& operator=(const app&) = delete;

    // Blocks on the main loop (UI run / idle park) until exit. Calls
    // shutdown() before returning. Throws std::runtime_error if a critical
    // server (HTTP API) fails to start.
    int run();

private:
    void wire_observers();
    void start_servers_and_workers();
    void wait_for_tree_init();
    void start_traffic_layers();
    int  run_main_loop();
    void shutdown() noexcept;

    void                                           sync_groups();
    [[nodiscard]] std::pair<std::string, uint16_t> pick_proxy_endpoint() const;
    [[nodiscard]] std::unique_ptr<webview_app>     create_gui();

    static constexpr int      API_PORT       = 18080;
    static constexpr uint16_t UDP_RELAY_PORT = 19999;

    // ---- Members in declaration / construction order ----

    cli_options opts_;
    HINSTANCE   hinstance_;

    config_manager config_;

    asio::io_context                              ioc_;
    asio::strand<asio::io_context::executor_type> strand_;
    int                                           num_workers_ = 0;

    bool              init_interrupted_ = false;
    std::atomic<bool> shut_down_        = false;

    config_store                 cfg_store_;
    domain::process_tree_manager tree_mgr_;
    strand_bound_manager         exec_;

    std::unique_ptr<PortTracker>                      port_tracker_ = std::make_unique<PortTracker>();
    std::unordered_map<uint32_t, ProxyGroupConfig>    tcp_groups_;
    std::unordered_map<uint32_t, UdpProxyGroupConfig> udp_groups_;
    DnsManager                                        dns_mgr_;
    async_acceptor                                    acceptor_;
    uint16_t                                          redirect_port_ = 0;
    std::unique_ptr<windivert_socket>                 wd_socket_;
    std::unique_ptr<windivert_network>                wd_network_;

    std::unique_ptr<UdpPortTracker>       udp_port_tracker_ = std::make_unique<UdpPortTracker>();
    UdpSessionTable                       udp_session_table_;
    std::unique_ptr<windivert_socket_udp>  wd_socket_udp_;
    std::unique_ptr<windivert_network_udp> wd_network_udp_;
    Socks5UdpManager                       socks5_udp_mgr_;
    std::unique_ptr<UdpRelay>              udp_relay_;

    icon_cache icons_;

    process_projection projection_;
    config_sse_bridge  cfg_bridge_;

    config_service       config_svc_;
    connection_service   connection_svc_;
    group_service        group_svc_;
    icon_service         icon_svc_;
    process_tree_service process_svc_;
    rule_service         rule_svc_;
    stats_service        stats_svc_;

    api_context ctx_;

    transport::http_api_server api_server_;

    std::unique_ptr<webview_app> gui_;
    std::vector<std::jthread>    workers_;
};

} // namespace clew
