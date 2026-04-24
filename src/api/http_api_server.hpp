#pragma once

// Public interface for the HTTP API + SSE server.
// Non-template implementation lives in http_api_server.cpp;
// strand_sync<> stays inline here because a template method can't be
// explicitly instantiated without knowing the caller's closure type.

#include <httplib.h>
#include <nlohmann/json.hpp>
#include "core/log.hpp"
#include <thread>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define ASIO_STANDALONE
#include <asio.hpp>

#include "process/process_tree_manager.hpp"
#include "config/config_manager.hpp"
#include "core/port_tracker.hpp"
#include "process/tcp_table.hpp"
#include "process/udp_table.hpp"
#include "udp/udp_port_tracker.hpp"
#include "api/icon_cache.hpp"

namespace clew {

using json = nlohmann::json;

// Given a hijacked connection, resolve its proxy_status field value
// ("PROXIED" / "IGNORED") based on the matched rule's traffic filter.
// Falls back to "PROXIED" if the rule-id lookup fails (auto-matched but
// rule was removed concurrently).
inline std::string resolve_proxy_filter_status(const rule_engine_v3& rules,
                                                const flat_tree& tree,
                                                DWORD pid,
                                                const std::string& remote_ip,
                                                uint16_t remote_port) {
    auto match = rules.get_match_info(tree, pid);
    if (!match) return "PROXIED";
    for (const auto& rule : rules.auto_rules()) {
        if (rule.id == match->rule_id) {
            return TrafficFilterEngine::should_proxy(remote_ip, remote_port, rule.dst_filter)
                 ? "PROXIED" : "IGNORED";
        }
    }
    return "PROXIED";
}

class http_api_server {
private:
    httplib::Server server_;
    std::jthread server_thread_;
    std::atomic<bool> running_{false};
    int port_;
    std::string static_dir_;

    process_tree_manager& mgr_;
    config_manager& config_manager_;
    asio::strand<asio::io_context::executor_type>& strand_;
    PortTracker* port_tracker_ = nullptr;
    UdpPortTracker* udp_port_tracker_ = nullptr;
    icon_cache* icon_cache_ = nullptr;

    // Atomic snapshot of the process tree JSON, published from strand.
    // Read from httplib threads lock-free.
    std::atomic<std::shared_ptr<const std::string>> tree_snapshot_{
        std::make_shared<const std::string>("[]")
    };

    // SSE client management
    std::mutex sse_mutex_;
    std::vector<httplib::DataSink*> sse_clients_;

    // Config change callback (for external wiring, e.g. WinDivert reload)
    std::function<void(const ConfigV2&)> on_config_change_;

    // ---- Helpers (implementations in .cpp) ----

    static std::string get_executable_dir();
    static std::string hijack_source_from_match(const std::optional<RuleMatchResult>& match);
    static std::pair<std::string, std::string> query_process_detail(DWORD pid);

    json entry_to_api_json(const flat_tree& tree, const rule_engine_v3& rules,
                           uint32_t idx, bool recursive) const;
    std::string build_tree_snapshot();
    void publish_tree_snapshot();

    void setup_static_files();
    void setup_routes();

    // Post work to the strand and wait for result synchronously (for httplib handlers).
    // Uses std::packaged_task: exception handling (including non-std::exception-derived
    // types) is delegated to the standard library — no explicit catch(...) needed.
    // Template → must stay inline here.
    template<typename Fn>
    auto strand_sync(Fn&& fn) -> decltype(fn()) {
        using R = decltype(fn());
        std::packaged_task<R()> task(std::forward<Fn>(fn));
        auto future = task.get_future();
        asio::post(strand_, [t = std::move(task)]() mutable { t(); });
        return future.get();
    }

public:
    http_api_server(int port,
                    process_tree_manager& mgr,
                    config_manager& cm,
                    asio::strand<asio::io_context::executor_type>& strand,
                    const std::string& static_dir = "");

    void set_static_dir(std::string_view dir);
    void set_port_tracker(PortTracker* pt);
    void set_udp_port_tracker(UdpPortTracker* pt);
    void set_icon_cache(icon_cache* ic);
    void set_on_config_change(std::function<void(const ConfigV2&)> cb);

    bool start();
    void stop();
    int get_port() const;

    // Broadcast an SSE event to all connected clients (thread-safe).
    void broadcast_event(const std::string& event, const json& data);

    // Convenience: broadcast process exit (called from strand on ETW ProcessStop).
    void broadcast_process_exit(DWORD pid);

    // Called from process_tree_manager's on_tree_changed callback (runs on strand).
    // Publishes atomic snapshot and fires SSE.
    void on_tree_changed();
};

} // namespace clew
