#pragma once

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#endif

#include <shellapi.h>
#include <commdlg.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "core/log.hpp"
#include <format>
#include <thread>
#include <atomic>
#include <functional>
#include <future>
#include <memory>

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

    // ================================================================
    // Helpers
    // ================================================================

    std::string get_executable_dir() {
        char path[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::string exe_path(path);
            size_t last_slash = exe_path.find_last_of("\\/");
            if (last_slash != std::string::npos) {
                return exe_path.substr(0, last_slash);
            }
        }
        return ".";
    }

    // Build hijack_source string from rule match info
    static std::string hijack_source_from_match(const std::optional<RuleMatchResult>& match) {
        if (!match) return "";
        if (match->rule_type == "manual") return "manual";
        if (match->rule_type == "auto") return "auto:" + match->rule_id;
        if (match->rule_type == "tree-inherited") return "tree:" + match->rule_id;
        return match->rule_type;
    }

    // Query process detail from OS (image_path + cmdline). Called on httplib thread, not strand.
    static std::pair<std::string, std::string> query_process_detail(DWORD pid) {
        std::string image_path;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h || h == INVALID_HANDLE_VALUE) return {image_path, {}};

        // image_path via QueryFullProcessImageNameW
        WCHAR path_buf[MAX_PATH];
        DWORD path_len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path_buf, &path_len)) {
            int utf8_len = WideCharToMultiByte(CP_UTF8, 0, path_buf, path_len, nullptr, 0, nullptr, nullptr);
            if (utf8_len > 0) {
                image_path.resize(utf8_len);
                WideCharToMultiByte(CP_UTF8, 0, path_buf, path_len, image_path.data(), utf8_len, nullptr, nullptr);
            }
        }
        CloseHandle(h);

        // cmdline via shared query_process_cmdline() in flat_tree.hpp
        return {image_path, query_process_cmdline(pid)};
    }

    // Serialize a single flat_tree entry to JSON (for API responses).
    // Must be called on strand (accesses tree + rules).
    json entry_to_api_json(const flat_tree& tree, const rule_engine_v3& rules,
                           uint32_t idx, bool recursive) const {
        const auto& e = tree.at(idx);
        auto match = rules.get_match_info(tree, e.pid);

        json j;
        j["pid"] = e.pid;
        j["parent_pid"] = e.parent_pid;
        j["name"] = std::string(e.name_u8);
        j["hijacked"] = e.is_proxied();
        j["hijack_source"] = hijack_source_from_match(match);

        if (recursive) {
            json children = json::array();
            uint32_t child = e.first_child_index;
            while (child != INVALID_IDX) {
                if (tree.at(child).alive) {
                    children.push_back(entry_to_api_json(tree, rules, child, true));
                }
                child = tree.at(child).next_sibling_index;
            }
            if (!children.empty()) {
                j["children"] = std::move(children);
            }
        }
        return j;
    }

    // Build the full tree JSON snapshot (called on strand).
    // This replaces flat_tree::to_json() to include hijack_source for the frontend.
    std::string build_tree_snapshot() {
        const auto& tree = mgr_.tree();
        const auto& rules = mgr_.rules();
        json arr = json::array();
        auto roots = tree.get_roots();
        for (uint32_t idx : roots) {
            arr.push_back(entry_to_api_json(tree, rules, idx, true));
        }
        return arr.dump();
    }

    // Publish a new tree snapshot atomically (called on strand after tree changes).
    void publish_tree_snapshot() {
        auto snap = std::make_shared<const std::string>(build_tree_snapshot());
        tree_snapshot_.store(std::move(snap));
    }

    // Post work to the strand and wait for result synchronously (for httplib handlers).
    template<typename Fn>
    auto strand_sync(Fn&& fn) -> decltype(fn()) {
        using R = decltype(fn());
        std::promise<R> promise;
        auto future = promise.get_future();
        asio::post(strand_, [fn = std::forward<Fn>(fn), p = std::move(promise)]() mutable {
            try {
                if constexpr (std::is_void_v<R>) {
                    fn();
                    p.set_value();
                } else {
                    p.set_value(fn());
                }
            } catch (...) {
                p.set_exception(std::current_exception());
            }
        });
        return future.get();
    }

    // ================================================================
    // Static file serving
    // ================================================================

    void setup_static_files() {
        std::vector<std::string> possible_paths = {
            static_dir_,
            get_executable_dir() + "/frontend/dist",
            get_executable_dir() + "/../frontend/dist",
            "./frontend/dist",
            "../frontend/dist",
            "../../frontend/dist",
            // Legacy paths (v1 ui/)
            get_executable_dir() + "/ui/dist",
            "./ui/dist",
        };

        for (const auto& path : possible_paths) {
            if (path.empty()) continue;
            std::string index_path = path + "/index.html";
            if (GetFileAttributesA(index_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                if (server_.set_mount_point("/", path)) {
                    PC_LOG_INFO("Serving static files from: {}", path);
                    return;
                }
            }
        }

        PC_LOG_WARN("Static files directory not found. Web UI will not be available.");
        PC_LOG_WARN("Build the frontend with 'npm run build' in the frontend/ directory.");
    }

    // ================================================================
    // Route setup
    // ================================================================

    void setup_routes() {
        // --- CORS ---
        server_.set_default_headers({
            {"Access-Control-Allow-Origin", "*"},
            {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
            {"Access-Control-Allow-Headers", "Content-Type, Authorization"}
        });

        server_.Options(".*", [](const httplib::Request&, httplib::Response& res) {
            res.status = 204;
        });

        // Auth middleware: when enabled, require Authorization: Bearer <token>
        // for all /api/... routes except /api/bootstrap (used by clients to discover auth state).
        server_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            using enum httplib::Server::HandlerResponse;
            if (!req.path.starts_with("/api/")) return Unhandled;
            if (req.path == "/api/bootstrap") return Unhandled;

            const auto& auth = config_manager_.get_v2().auth;
            if (!auth.enabled || auth.token.empty()) return Unhandled;

            // Accept token via Authorization: Bearer <token>
            auto h = req.get_header_value("Authorization");
            constexpr std::string_view prefix = "Bearer ";
            if (h.size() > prefix.size() && std::string_view(h).substr(0, prefix.size()) == prefix
                && std::string_view(h).substr(prefix.size()) == auth.token) {
                return Unhandled;
            }
            // Fallback: ?token=... query parameter (needed for EventSource/SSE which
            // cannot set custom headers).
            if (req.has_param("token") && req.get_param_value("token") == auth.token) {
                return Unhandled;
            }

            res.status = 401;
            res.set_content(R"({"error":"unauthorized"})", "application/json");
            return Handled;
        });

        // Prevent WebView2 from caching static files (index.html especially)
        server_.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
            if (req.path == "/" || req.path.ends_with(".html")) {
                res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
            }
        });

        // GET /api/bootstrap — auth discovery endpoint (no token required).
        // Frontend calls this first to learn whether to attach Authorization header.
        server_.Get("/api/bootstrap", [this](const httplib::Request&, httplib::Response& res) {
            const auto& auth = config_manager_.get_v2().auth;
            json j;
            j["auth_enabled"] = auth.enabled && !auth.token.empty();
            res.set_content(j.dump(), "application/json");
        });

        setup_static_files();

        // =============================================================
        // Process APIs
        // =============================================================

        // GET /api/processes — Full process tree (lock-free atomic snapshot read)
        server_.Get("/api/processes", [this](const httplib::Request&, httplib::Response& res) {
            auto snap = tree_snapshot_.load();
            res.set_content(*snap, "application/json");
        });

        // GET /api/processes/:pid — Single process info
        server_.Get(R"(/api/processes/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            DWORD pid = std::stoul(req.matches[1]);
            try {
                auto result = strand_sync([this, pid]() -> json {
                    const auto& tree = mgr_.tree();
                    uint32_t idx = tree.find_by_pid(pid);
                    if (idx == INVALID_IDX) return json{};
                    return entry_to_api_json(tree, mgr_.rules(), idx, true);
                });
                if (result.empty()) {
                    res.status = 404;
                    res.set_content(R"({"error":"Process not found"})", "application/json");
                } else {
                    res.set_content(result.dump(), "application/json");
                }
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // GET /api/processes/:pid/detail — Process detail with cmdline + image_path
        server_.Get(R"(/api/processes/(\d+)/detail)", [this](const httplib::Request& req, httplib::Response& res) {
            DWORD pid = std::stoul(req.matches[1]);
            try {
                auto result = strand_sync([this, pid]() -> json {
                    const auto& tree = mgr_.tree();
                    uint32_t idx = tree.find_by_pid(pid);
                    if (idx == INVALID_IDX) return json{};
                    return entry_to_api_json(tree, mgr_.rules(), idx, true);
                });
                if (result.empty()) {
                    res.status = 404;
                    res.set_content(R"({"error":"Process not found"})", "application/json");
                } else {
                    // Query OS for cmdline + image_path (outside strand, on httplib thread)
                    auto [image_path, cmdline] = query_process_detail(pid);
                    result["image_path"] = image_path;
                    result["cmdline"] = cmdline;
                    res.set_content(result.dump(), "application/json");
                }
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // =============================================================
        // Hijack APIs
        // =============================================================

        // GET /api/hijack — List all hijacked PIDs
        server_.Get("/api/hijack", [this](const httplib::Request&, httplib::Response& res) {
            try {
                auto result = strand_sync([this]() -> json {
                    const auto& tree = mgr_.tree();
                    const auto& rules = mgr_.rules();
                    auto pids = rules.get_hijacked_pids(tree);
                    json j = json::array();
                    for (DWORD pid : pids) {
                        uint32_t idx = tree.find_by_pid(pid);
                        if (idx == INVALID_IDX) continue;
                        const auto& e = tree.at(idx);
                        auto match = rules.get_match_info(tree, pid);
                        json pj;
                        pj["pid"] = pid;
                        pj["name"] = std::string(e.name_u8);
                        pj["hijacked"] = true;
                        pj["hijack_source"] = hijack_source_from_match(match);
                        j.push_back(pj);
                    }
                    return j;
                });
                res.set_content(result.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // POST /api/hijack/:pid — Manual hijack (default: tree mode)
        // Optional body: {"tree": true/false, "group_id": 0}
        server_.Post(R"(/api/hijack/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            DWORD pid = std::stoul(req.matches[1]);
            bool tree_mode = true;
            uint32_t group_id = 0;
            if (!req.body.empty()) {
                auto body = json::parse(req.body, nullptr, false);
                if (!body.is_discarded()) {
                    tree_mode = body.value("tree", true);
                    group_id = body.value("group_id", 0u);
                }
            }
            try {
                auto result = strand_sync([this, pid, tree_mode, group_id]() -> json {
                    auto& tree = mgr_.tree();
                    uint32_t idx = tree.find_by_pid(pid);
                    if (idx == INVALID_IDX) {
                        return json{{"error", "Process not found"}};
                    }

                    if (tree_mode) {
                        mgr_.rules().manual_hijack_tree(tree, pid, group_id);
                    } else {
                        mgr_.rules().manual_hijack(tree, pid, group_id);
                    }

                    publish_tree_snapshot();

                    const auto& e = tree.at(idx);
                    json evt;
                    evt["pid"] = pid;
                    evt["name"] = std::string(e.name_u8);
                    evt["hijacked"] = true;
                    evt["hijack_source"] = "manual";
                    broadcast_event("process_update", evt);

                    return json{{"success", true}};
                });
                if (result.contains("error")) {
                    res.status = 404;
                    res.set_content(result.dump(), "application/json");
                } else {
                    res.set_content(result.dump(), "application/json");
                }
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // DELETE /api/hijack/:pid — Manual unhijack
        // Default: single PID only. ?tree=true for cascade unhijack.
        server_.Delete(R"(/api/hijack/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            DWORD pid = std::stoul(req.matches[1]);
            bool tree_mode = req.has_param("tree") && req.get_param_value("tree") == "true";
            auto t0 = std::chrono::high_resolution_clock::now();
            try {
                strand_sync([this, pid, tree_mode, t0]() {
                    auto t1 = std::chrono::high_resolution_clock::now();
                    auto& tree = mgr_.tree();
                    if (tree_mode) {
                        mgr_.rules().manual_unhijack_tree(tree, pid);
                    } else {
                        mgr_.rules().manual_unhijack(tree, pid);
                    }
                    auto t2 = std::chrono::high_resolution_clock::now();

                    publish_tree_snapshot();
                    auto t3 = std::chrono::high_resolution_clock::now();

                    json evt;
                    evt["pid"] = pid;
                    evt["hijacked"] = false;
                    broadcast_event("process_update", evt);
                    auto t4 = std::chrono::high_resolution_clock::now();

                    auto us = [](auto a, auto b) {
                        return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
                    };
                    PC_LOG_DEBUG("[UNHIJACK] PID={} tree={} strand_wait={}us unhijack={}us snapshot={}us sse={}us total={}us",
                                 pid, tree_mode, us(t0,t1), us(t1,t2), us(t2,t3), us(t3,t4), us(t0,t4));
                });
                res.set_content(R"({"success":true})", "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // POST /api/hijack/batch — Batch hijack/unhijack (single HTTP request for tree operations)
        // Body: {"pids": [1,2,3], "action": "hijack"|"unhijack", "group_id": 0}
        server_.Post("/api/hijack/batch", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                auto pids = body["pids"].get<std::vector<DWORD>>();
                std::string action = body.value("action", "hijack");
                uint32_t group_id = body.value("group_id", 0u);

                strand_sync([this, pids, action, group_id]() {
                    auto& tree = mgr_.tree();
                    auto& rules = mgr_.rules();
                    for (DWORD pid : pids) {
                        if (action == "unhijack") {
                            rules.manual_unhijack(tree, pid);
                        } else {
                            rules.manual_hijack(tree, pid, group_id);
                        }
                    }
                    // Single snapshot rebuild for entire batch
                    publish_tree_snapshot();
                    broadcast_event("process_update", json{{"action", action}, {"count", pids.size()}});
                });
                res.set_content(R"({"success":true})", "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // =============================================================
        // System TCP Connections API
        // =============================================================

        server_.Get("/api/tcp", [this](const httplib::Request& req, httplib::Response& res) {
            DWORD filter_pid = 0;
            if (req.has_param("pid")) {
                filter_pid = std::stoul(req.get_param_value("pid"));
            }

            auto connections = tcp_table::get_connections(filter_pid);

            // We need tree access for process names + hijack status.
            // Use strand_sync for consistent read.
            try {
                auto result = strand_sync([this, &connections, filter_pid]() -> json {
                    json j = json::array();
                    const auto& tree = mgr_.tree();
                    const auto& rules = mgr_.rules();

                    for (const auto& conn : connections) {
                        // When showing all connections (no PID filter), skip LISTEN/unconnected
                        if (filter_pid == 0 &&
                            (conn.state == "LISTEN" ||
                             (conn.remote_ip == "0.0.0.0" && conn.remote_port == 0))) {
                            continue;
                        }

                        json c;
                        c["pid"] = conn.pid;
                        c["local_ip"] = conn.local_ip;
                        c["local_port"] = conn.local_port;
                        c["remote_ip"] = conn.remote_ip;
                        c["remote_port"] = conn.remote_port;
                        c["state"] = conn.state;
                        c["dest"] = std::format("{}:{}", conn.remote_ip, conn.remote_port);

                        // Check hijack status from flat_tree entry
                        uint32_t idx = tree.find_by_pid(conn.pid);
                        bool pid_alive = (idx != INVALID_IDX);
                        bool is_hijacked = false;
                        if (pid_alive) {
                            is_hijacked = tree.at(idx).is_proxied();
                        }
                        c["hijacked"] = is_hijacked;
                        c["pid_alive"] = pid_alive;

                        if (!is_hijacked) {
                            c["proxy_status"] = "-";
                        } else if (conn.state == "LISTEN" || (conn.remote_ip == "0.0.0.0" && conn.remote_port == 0)) {
                            c["proxy_status"] = "LISTEN";
                        } else {
                            c["proxy_status"] = resolve_proxy_filter_status(
                                rules, tree, conn.pid, conn.remote_ip, conn.remote_port);
                        }

                        // Process name from tree
                        if (idx != INVALID_IDX) {
                            c["process_name"] = std::string(tree.at(idx).name_u8);
                        } else {
                            c["process_name"] = "unknown";
                        }

                        j.push_back(c);
                    }
                    return j;
                });
                res.set_content(result.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // GET /api/udp — UDP endpoints (enriched with UdpPortTracker remote info)
        server_.Get("/api/udp", [this](const httplib::Request& req, httplib::Response& res) {
            DWORD filter_pid = 0;
            if (req.has_param("pid")) {
                filter_pid = std::stoul(req.get_param_value("pid"));
            }

            auto endpoints = udp_table::get_endpoints(filter_pid);

            try {
                auto result = strand_sync([this, &endpoints, filter_pid]() -> json {
                    json j = json::array();
                    const auto& tree = mgr_.tree();
                    const auto& rules = mgr_.rules();

                    for (const auto& ep : endpoints) {
                        // Skip port 0 entries (not actually bound)
                        if (ep.local_port == 0 && filter_pid == 0) continue;

                        json c;
                        c["pid"] = ep.pid;
                        c["local_ip"] = ep.local_ip;
                        c["local_port"] = ep.local_port;
                        c["state"] = "BOUND";

                        // Enrich with UdpPortTracker: get remote addr for hijacked entries
                        std::string remote_ip;
                        uint16_t remote_port = 0;
                        if (udp_port_tracker_ && udp_port_tracker_->is_active(ep.local_port)) {
                            const auto& entry = udp_port_tracker_->peek(ep.local_port);
                            // Convert host byte order IPv4 to string
                            struct in_addr addr;
                            addr.s_addr = htonl(entry.remote_addr[0]);
                            char buf[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &addr, buf, sizeof(buf));
                            remote_ip = buf;
                            remote_port = entry.remote_port;
                        }
                        c["remote_ip"] = remote_ip;
                        c["remote_port"] = remote_port;
                        c["dest"] = remote_ip.empty() ? std::string{} : std::format("{}:{}", remote_ip, remote_port);

                        // Hijack/process info from flat_tree
                        uint32_t idx = tree.find_by_pid(ep.pid);
                        bool pid_alive = (idx != INVALID_IDX);
                        bool is_hijacked = false;
                        if (pid_alive) {
                            is_hijacked = tree.at(idx).is_proxied();
                        }
                        c["hijacked"] = is_hijacked;
                        c["pid_alive"] = pid_alive;

                        if (!is_hijacked) {
                            c["proxy_status"] = "-";
                        } else {
                            c["proxy_status"] = resolve_proxy_filter_status(
                                rules, tree, ep.pid, remote_ip, remote_port);
                        }

                        if (idx != INVALID_IDX) {
                            c["process_name"] = std::string(tree.at(idx).name_u8);
                        } else {
                            c["process_name"] = "unknown";
                        }

                        j.push_back(c);
                    }
                    return j;
                });
                res.set_content(result.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // =============================================================
        // Auto Rules API
        // =============================================================

        // GET /api/auto-rules — List all auto rules with runtime state
        server_.Get("/api/auto-rules", [this](const httplib::Request&, httplib::Response& res) {
            try {
                auto result = strand_sync([this]() -> json {
                    const auto& tree = mgr_.tree();
                    const auto& rules = mgr_.rules().auto_rules();
                    json j = json::array();
                    for (const auto& rule : rules) {
                        json rj = rule;  // Uses AutoRule to_json
                        // Add runtime state
                        rj["matched_count"] = rule.matched_pids.size();
                        rj["excluded_count"] = rule.excluded_pids.size();
                        rj["matched_pids"] = json::array();
                        for (DWORD pid : rule.matched_pids) {
                            json pj;
                            pj["pid"] = pid;
                            uint32_t idx = tree.find_by_pid(pid);
                            if (idx != INVALID_IDX) {
                                pj["name"] = std::string(tree.at(idx).name_u8);
                            } else {
                                pj["name"] = "unknown";
                            }
                            pj["excluded"] = rule.excluded_pids.contains(pid);
                            rj["matched_pids"].push_back(pj);
                        }
                        j.push_back(rj);
                    }
                    return j;
                });
                res.set_content(result.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // POST /api/auto-rules — Create auto rule
        server_.Post("/api/auto-rules", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                AutoRule new_rule = body.get<AutoRule>();

                // Generate id if not provided (frontend creates don't send id)
                if (new_rule.id.empty()) {
                    new_rule.id = std::format("rule_{}",
                        std::chrono::steady_clock::now().time_since_epoch().count());
                }

                auto result = strand_sync([this, rule = std::move(new_rule)]() mutable -> json {
                    auto& rules = mgr_.rules();
                    rules.auto_rules().push_back(std::move(rule));

                    // Re-apply all auto rules to pick up new rule matches
                    rules.apply_auto_rules(mgr_.tree());

                    // Save to config
                    config_manager_.get_v2().auto_rules = rules.auto_rules();
                    config_manager_.save();

                    // Publish updated snapshot + SSE
                    publish_tree_snapshot();
                    broadcast_event("auto_rule_changed", json{{"action", "created"}});

                    return json{{"success", true}};
                });
                res.set_content(result.dump(), "application/json");
            } catch (const json::exception& e) {
                res.status = 400;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // PUT /api/auto-rules/:id — Partial update auto rule
        server_.Put(R"(/api/auto-rules/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            try {
                auto body = json::parse(req.body);

                auto result = strand_sync([this, id, body]() -> json {
                    auto& rules_vec = mgr_.rules().auto_rules();

                    // Find the rule
                    AutoRule* found = nullptr;
                    for (auto& r : rules_vec) {
                        if (r.id == id) { found = &r; break; }
                    }
                    if (!found) {
                        return json{{"error", "Rule not found"}};
                    }

                    // Merge fields from body
                    if (body.contains("name")) found->name = body["name"];
                    if (body.contains("enabled")) found->enabled = body["enabled"];
                    if (body.contains("process_name")) found->process_name = body["process_name"];
                    if (body.contains("cmdline_pattern")) found->cmdline_pattern = body["cmdline_pattern"];
                    if (body.contains("hack_tree")) found->hack_tree = body["hack_tree"];
                    if (body.contains("proxy_group_id")) found->proxy_group_id = body["proxy_group_id"];
                    if (body.contains("protocol")) found->protocol = body["protocol"];
                    if (body.contains("proxy")) found->proxy = body["proxy"].get<ProxyTarget>();
                    if (body.contains("dst_filter")) found->dst_filter = body["dst_filter"].get<TrafficFilter>();

                    // Re-apply all auto rules
                    mgr_.rules().apply_auto_rules(mgr_.tree());

                    // Save to config
                    config_manager_.get_v2().auto_rules = mgr_.rules().auto_rules();
                    config_manager_.save();

                    // Publish updated snapshot + SSE
                    publish_tree_snapshot();
                    broadcast_event("auto_rule_changed", json{{"action", "updated"}, {"id", id}});

                    return json{{"success", true}};
                });

                if (result.contains("error")) {
                    res.status = 404;
                    res.set_content(result.dump(), "application/json");
                } else {
                    res.set_content(result.dump(), "application/json");
                }
            } catch (const json::exception& e) {
                res.status = 400;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // DELETE /api/auto-rules/:id — Delete auto rule
        server_.Delete(R"(/api/auto-rules/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            auto t0 = std::chrono::steady_clock::now();
            try {
                auto result = strand_sync([this, id]() -> json {
                    auto& rules_vec = mgr_.rules().auto_rules();
                    auto it = std::find_if(rules_vec.begin(), rules_vec.end(),
                                           [&](const AutoRule& r) { return r.id == id; });
                    if (it == rules_vec.end()) {
                        return json{{"error", "Rule not found"}};
                    }
                    rules_vec.erase(it);

                    // Re-apply remaining rules
                    mgr_.rules().apply_auto_rules(mgr_.tree());

                    // Save to config
                    config_manager_.get_v2().auto_rules = mgr_.rules().auto_rules();
                    config_manager_.save();

                    // Publish updated snapshot + SSE
                    publish_tree_snapshot();
                    broadcast_event("auto_rule_changed", json{{"action", "deleted"}, {"id", id}});

                    return json{{"success", true}};
                });

                auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                PC_LOG_DEBUG("[API] DELETE /api/auto-rules/{} completed in {}us", id, elapsed);

                if (result.contains("error")) {
                    res.status = 404;
                    res.set_content(result.dump(), "application/json");
                } else {
                    res.set_content(result.dump(), "application/json");
                }
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // POST /api/auto-rules/:id/exclude/:pid — Exclude PID from auto rule
        server_.Post(R"(/api/auto-rules/([^/]+)/exclude/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            DWORD pid = std::stoul(req.matches[2]);
            try {
                auto result = strand_sync([this, id, pid]() -> json {
                    if (mgr_.rules().exclude_pid(mgr_.tree(), id, pid)) {
                        publish_tree_snapshot();
                        broadcast_event("auto_rule_changed", json{{"action", "exclude"}, {"id", id}, {"pid", pid}});
                        return json{{"success", true}};
                    }
                    return json{{"error", "Rule not found"}};
                });

                if (result.contains("error")) {
                    res.status = 404;
                    res.set_content(result.dump(), "application/json");
                } else {
                    res.set_content(result.dump(), "application/json");
                }
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // DELETE /api/auto-rules/:id/exclude/:pid — Unexclude PID from auto rule
        server_.Delete(R"(/api/auto-rules/([^/]+)/exclude/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            std::string id = req.matches[1];
            DWORD pid = std::stoul(req.matches[2]);
            try {
                auto result = strand_sync([this, id, pid]() -> json {
                    if (mgr_.rules().unexclude_pid(id, pid)) {
                        // Re-apply rules so the unexcluded PID gets picked up again
                        mgr_.rules().apply_auto_rules(mgr_.tree());
                        publish_tree_snapshot();
                        broadcast_event("auto_rule_changed", json{{"action", "unexclude"}, {"id", id}, {"pid", pid}});
                        return json{{"success", true}};
                    }
                    return json{{"error", "Rule not found"}};
                });

                if (result.contains("error")) {
                    res.status = 404;
                    res.set_content(result.dump(), "application/json");
                } else {
                    res.set_content(result.dump(), "application/json");
                }
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // =============================================================
        // Proxy Groups CRUD
        // =============================================================

        // GET /api/proxy-groups — List all proxy groups
        server_.Get("/api/proxy-groups", [this](const httplib::Request&, httplib::Response& res) {
            const auto& groups = config_manager_.get_proxy_groups();
            json j = json::array();
            for (const auto& g : groups) {
                j.push_back(json{
                    {"id", g.id},
                    {"name", g.name},
                    {"host", g.host},
                    {"port", g.port},
                    {"type", g.type},
                    {"test_url", g.test_url}
                });
            }
            res.set_content(j.dump(), "application/json");
        });

        // POST /api/proxy-groups — Create a new proxy group
        server_.Post("/api/proxy-groups", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                ProxyGroup group;
                group.id = config_manager_.get_v2().next_group_id++;
                group.name = body.value("name", std::format("group_{}", group.id));
                group.host = body.value("host", "127.0.0.1");
                group.port = body.value("port", 7890);
                group.type = body.value("type", "socks5");

                config_manager_.get_v2().proxy_groups.push_back(group);
                config_manager_.save();

                json result = group;
                res.set_content(result.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // PUT /api/proxy-groups/:id — Update a proxy group
        server_.Put(R"(/api/proxy-groups/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            uint32_t group_id = std::stoul(req.matches[1]);
            try {
                auto body = json::parse(req.body);
                auto& groups = config_manager_.get_v2().proxy_groups;
                ProxyGroup* found = nullptr;
                for (auto& g : groups) {
                    if (g.id == group_id) { found = &g; break; }
                }
                if (!found) {
                    res.status = 404;
                    res.set_content(R"({"error":"Proxy group not found"})", "application/json");
                    return;
                }

                if (body.contains("name")) found->name = body["name"];
                if (body.contains("host")) found->host = body["host"];
                if (body.contains("port")) found->port = body["port"];
                if (body.contains("type")) found->type = body["type"];
                if (body.contains("test_url")) found->test_url = body["test_url"];

                config_manager_.save();
                res.set_content(R"({"success":true})", "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // DELETE /api/proxy-groups/:id — Delete a proxy group (409 if in use)
        server_.Delete(R"(/api/proxy-groups/(\d+))", [this](const httplib::Request& req, httplib::Response& res) {
            uint32_t group_id = std::stoul(req.matches[1]);

            if (group_id == 0) {
                res.status = 400;
                res.set_content(R"({"error":"Cannot delete the default proxy group"})", "application/json");
                return;
            }

            auto& groups = config_manager_.get_v2().proxy_groups;
            auto it = std::find_if(groups.begin(), groups.end(),
                                   [group_id](const ProxyGroup& g) { return g.id == group_id; });
            if (it == groups.end()) {
                res.status = 404;
                res.set_content(R"({"error":"Proxy group not found"})", "application/json");
                return;
            }

            // Check auto rules references
            json referencing_rules = json::array();
            for (const auto& rule : config_manager_.get_v2().auto_rules) {
                if (rule.proxy_group_id == group_id) {
                    referencing_rules.push_back(json{{"id", rule.id}, {"name", rule.name}});
                }
            }

            // Check manual hijacks on strand
            int manual_count = 0;
            try {
                manual_count = strand_sync([this, group_id]() -> int {
                    int count = 0;
                    for (const auto& e : mgr_.tree().entries()) {
                        if (e.alive && e.group_id == group_id &&
                            e.has_flag(entry_flags::MANUAL_HIJACK))
                            count++;
                    }
                    return count;
                });
            } catch (...) {
                PC_LOG_WARN("[API] strand_sync threw during manual_count query");
            }

            if (!referencing_rules.empty() || manual_count > 0) {
                res.status = 409;
                res.set_content(json{
                    {"error", "group_in_use"},
                    {"auto_rules", referencing_rules},
                    {"manual_hijack_count", manual_count}
                }.dump(), "application/json");
                return;
            }

            groups.erase(it);
            config_manager_.save();
            res.set_content(R"({"success":true})", "application/json");
        });

        // POST /api/proxy-groups/:id/migrate — Migrate refs to target group, then delete
        server_.Post(R"(/api/proxy-groups/(\d+)/migrate)", [this](const httplib::Request& req, httplib::Response& res) {
            uint32_t source_id = std::stoul(req.matches[1]);
            if (source_id == 0) {
                res.status = 400;
                res.set_content(R"({"error":"Cannot migrate the default group"})", "application/json");
                return;
            }
            try {
                auto body = json::parse(req.body);
                uint32_t target_id = body.value("target_group_id", 0u);

                const auto& cfg = config_manager_.get_v2();
                bool source_exists = false;
                bool target_exists = false;
                for (const auto& g : cfg.proxy_groups) {
                    if (g.id == source_id) source_exists = true;
                    if (g.id == target_id) target_exists = true;
                }
                if (!source_exists) { res.status = 404; res.set_content(R"({"error":"Source group not found"})", "application/json"); return; }
                if (!target_exists) { res.status = 400; res.set_content(R"({"error":"Target group not found"})", "application/json"); return; }

                strand_sync([this, source_id, target_id]() {
                    auto& cfg = config_manager_.get_v2();
                    // Move auto rules
                    for (auto& rule : cfg.auto_rules) {
                        if (rule.proxy_group_id == source_id)
                            rule.proxy_group_id = target_id;
                    }
                    // Move manual hijacks in flat_tree
                    for (auto& e : mgr_.tree().entries()) {
                        if (e.alive && e.group_id == source_id &&
                            e.has_flag(entry_flags::MANUAL_HIJACK))
                            e.group_id = target_id;
                    }
                    // Re-apply rules
                    mgr_.rules().set_auto_rules(cfg.auto_rules);
                    mgr_.rules().apply_auto_rules(mgr_.tree());
                    // Delete source group
                    std::erase_if(cfg.proxy_groups,
                                  [source_id](const ProxyGroup& g) { return g.id == source_id; });
                    config_manager_.save();
                    publish_tree_snapshot();
                    broadcast_event("auto_rule_changed", json{{"action", "migrated"}});
                });

                res.set_content(R"({"success":true})", "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // POST /api/proxy-groups/:id/test — SOCKS5 connectivity test (raw socket + HTTP HEAD)
        server_.Post(R"(/api/proxy-groups/(\d+)/test)", [this](const httplib::Request& req, httplib::Response& res) {
            uint32_t group_id = std::stoul(req.matches[1]);

            const ProxyGroup* group = nullptr;
            for (const auto& g : config_manager_.get_v2().proxy_groups) {
                if (g.id == group_id) { group = &g; break; }
            }
            if (!group) {
                res.status = 404;
                res.set_content(R"({"error":"Group not found"})", "application/json");
                return;
            }

            // Parse test_url → host + port + path
            // Always use port 80 for HTTP HEAD test (HTTPS port 443 expects TLS, can't test with plain HTTP)
            std::string test_host;
            std::string test_path = "/";
            uint16_t test_port = 80;
            {
                std::string url = group->test_url;
                auto pos = url.find("://");
                if (pos != std::string::npos) {
                    url = url.substr(pos + 3);
                }
                pos = url.find('/');
                if (pos != std::string::npos) {
                    test_path = url.substr(pos);
                    url = url.substr(0, pos);
                }
                pos = url.find(':');
                if (pos != std::string::npos) {
                    test_host = url.substr(0, pos);
                    test_port = static_cast<uint16_t>(std::stoi(url.substr(pos + 1)));
                } else {
                    test_host = url;
                }
            }

            auto t0 = std::chrono::steady_clock::now();

            SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock == INVALID_SOCKET) {
                res.set_content(R"({"error":"socket creation failed"})", "application/json");
                return;
            }
            DWORD timeout_ms = 5000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
            auto cleanup = [&]() { closesocket(sock); };

            // 1. TCP connect to SOCKS5 proxy
            sockaddr_in proxy_addr{};
            proxy_addr.sin_family = AF_INET;
            proxy_addr.sin_port = htons(group->port);
            inet_pton(AF_INET, group->host.c_str(), &proxy_addr.sin_addr);
            if (::connect(sock, (sockaddr*)&proxy_addr, sizeof(proxy_addr)) != 0) {
                cleanup();
                res.set_content(R"({"error":"proxy connect failed"})", "application/json");
                return;
            }

            // 2. SOCKS5 greeting
            uint8_t greeting[] = {0x05, 0x01, 0x00};
            if (::send(sock, (const char*)greeting, 3, 0) != 3) { cleanup(); res.set_content(R"({"error":"send failed"})", "application/json"); return; }
            uint8_t greeting_reply[2];
            if (::recv(sock, (char*)greeting_reply, 2, 0) != 2 || greeting_reply[1] != 0x00) { cleanup(); res.set_content(R"({"error":"socks5 auth failed"})", "application/json"); return; }

            // 3. SOCKS5 CONNECT to test_host:test_port
            std::vector<uint8_t> creq = {0x05, 0x01, 0x00, 0x03, (uint8_t)test_host.size()};
            creq.insert(creq.end(), test_host.begin(), test_host.end());
            creq.push_back((uint8_t)(test_port >> 8));
            creq.push_back((uint8_t)(test_port & 0xFF));
            if (::send(sock, (const char*)creq.data(), (int)creq.size(), 0) != (int)creq.size()) { cleanup(); res.set_content(R"({"error":"send connect failed"})", "application/json"); return; }
            uint8_t creply[10];
            int n = ::recv(sock, (char*)creply, 10, 0);
            if (n < 4 || creply[1] != 0x00) { cleanup(); res.set_content(json{{"error","socks5 connect failed"}}.dump(), "application/json"); return; }

            // 4. Send HTTP HEAD and wait for response (measures real RTT through proxy chain)
            std::string head_req = "HEAD " + test_path + " HTTP/1.1\r\nHost: " + test_host + "\r\nConnection: close\r\n\r\n";
            if (::send(sock, head_req.c_str(), (int)head_req.size(), 0) <= 0) { cleanup(); res.set_content(R"({"error":"send http failed"})", "application/json"); return; }
            char http_reply[32];
            int r = ::recv(sock, http_reply, sizeof(http_reply), 0);
            cleanup();

            if (r <= 0) {
                res.set_content(R"({"error":"timeout"})", "application/json");
                return;
            }

            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            PC_LOG_DEBUG("[API] proxy-groups/{}/test: {}ms to {}", group_id, elapsed_ms, group->test_url);
            res.set_content(json{{"latency_ms", elapsed_ms}}.dump(), "application/json");
        });

        // =============================================================
        // Config Raw API
        // =============================================================

        // GET /api/config — Raw JSON config (for Monaco editor)
        server_.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
            res.set_content(config_manager_.get_raw_config(), "application/json");
        });

        // PUT /api/config — Set raw JSON config (from Monaco editor), reload rules
        server_.Put("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
            std::string error = config_manager_.set_raw_config(req.body);
            if (error.empty()) {
                // Reload auto rules into rule engine on strand
                try {
                    strand_sync([this]() {
                        const auto& cfg = config_manager_.get_v2();
                        mgr_.rules().set_auto_rules(cfg.auto_rules);
                        mgr_.rules().apply_auto_rules(mgr_.tree());
                        publish_tree_snapshot();
                    });

                    if (on_config_change_) on_config_change_(config_manager_.get_v2());
                    broadcast_event("auto_rule_changed", json{{"action", "config_reload"}});
                    res.set_content(R"({"success":true})", "application/json");
                } catch (const std::exception& e) {
                    res.status = 500;
                    res.set_content(json{{"error", e.what()}}.dump(), "application/json");
                }
            } else {
                res.status = 400;
                res.set_content(json{{"error", error}}.dump(), "application/json");
            }
        });

        // =============================================================
        // Stats API
        // =============================================================

        server_.Get("/api/stats", [this](const httplib::Request&, httplib::Response& res) {
            try {
                auto result = strand_sync([this]() -> json {
                    json j;
                    const auto& tree = mgr_.tree();
                    j["hijacked_pids"] = mgr_.rules().get_hijacked_pids(tree).size();
                    j["auto_rules_count"] = mgr_.rules().auto_rules().size();
                    return j;
                });
                res.set_content(result.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // =============================================================
        // Icon API
        // =============================================================

        // GET /api/icon?name=chrome.exe — Returns 16x16 PNG icon
        server_.Get("/api/icon", [this](const httplib::Request& req, httplib::Response& res) {
            if (!icon_cache_ || !req.has_param("name")) {
                res.status = 400;
                return;
            }

            const auto& name = req.get_param_value("name");
            std::string path;
            if (req.has_param("path")) {
                path = req.get_param_value("path");
            }

            const auto& png = icon_cache_->get_icon_by_name(name, path);
            if (png.empty()) {
                res.status = 404;
                return;
            }

            res.set_content(std::string(reinterpret_cast<const char*>(png.data()), png.size()),
                           "image/png");
            res.set_header("Cache-Control", "public, max-age=86400");
        });

        // =============================================================
        // System APIs
        // =============================================================

        // GET /api/env
        server_.Get("/api/env", [](const httplib::Request&, httplib::Response& res) {
            json j;
            auto get_env = [](const char* name) -> std::string {
                // GetEnvironmentVariableA returns buffer size (including NUL)
                // on success, 0 on not-set / error. RAII via std::string.
                DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
                if (size == 0) return "";
                std::string result(size - 1, '\0');
                GetEnvironmentVariableA(name, result.data(), size);
                return result;
            };
            j["HTTP_PROXY"] = get_env("HTTP_PROXY");
            j["HTTPS_PROXY"] = get_env("HTTPS_PROXY");
            j["NO_PROXY"] = get_env("NO_PROXY");
            j["http_proxy"] = get_env("http_proxy");
            j["https_proxy"] = get_env("https_proxy");
            j["no_proxy"] = get_env("no_proxy");
            res.set_content(j.dump(), "application/json");
        });

        // =============================================================
        // Shell APIs
        // =============================================================

        // POST /api/shell/browse-exe — Native file picker
        server_.Post("/api/shell/browse-exe", [](const httplib::Request&, httplib::Response& res) {
            char file_path[MAX_PATH] = {0};
            OPENFILENAMEA ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = nullptr;
            ofn.lpstrFilter = "Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = file_path;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                std::string full_path(file_path);
                std::string dir;
                std::string name;
                size_t last_sep = full_path.find_last_of("\\/");
                if (last_sep != std::string::npos) {
                    dir = full_path.substr(0, last_sep + 1);
                    name = full_path.substr(last_sep + 1);
                } else {
                    name = full_path;
                }
                json j;
                j["path"] = full_path;
                j["dir"] = dir;
                j["name"] = name;
                res.set_content(j.dump(), "application/json");
            } else {
                res.set_content(R"({"cancelled":true})", "application/json");
            }
        });

        // POST /api/shell/reveal — Open explorer to file
        server_.Post("/api/shell/reveal", [](const httplib::Request& req, httplib::Response& res) {
            try {
                auto body = json::parse(req.body);
                std::string path = body.value("path", "");
                if (path.empty()) {
                    res.status = 400;
                    res.set_content(R"({"error":"path is required"})", "application/json");
                    return;
                }
                // Convert UTF-8 path to UTF-16 for proper Unicode support
                int needed = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
                std::wstring wpath(needed - 1, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), needed);
                std::wstring wcmd = L"/select,\"" + wpath + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", wcmd.c_str(), nullptr, SW_SHOWNORMAL);
                res.set_content(R"({"success":true})", "application/json");
            } catch (const std::exception& e) {
                res.status = 400;
                res.set_content(json{{"error", e.what()}}.dump(), "application/json");
            }
        });

        // =============================================================
        // SSE Endpoint
        // =============================================================

        server_.Get("/api/events", [this](const httplib::Request&, httplib::Response& res) {
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_chunked_content_provider("text/event-stream",
                [this](size_t, httplib::DataSink& sink) {
                    {
                        std::scoped_lock lock(sse_mutex_);
                        sse_clients_.push_back(&sink);
                    }
                    int keepalive_counter = 0;
                    while (running_ && sink.is_writable()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        if (++keepalive_counter >= 5) {  // keepalive every 1 second
                            sink.write(": keepalive\n\n", 13);
                            keepalive_counter = 0;
                        }
                    }
                    {
                        std::scoped_lock lock(sse_mutex_);
                        sse_clients_.erase(
                            std::remove(sse_clients_.begin(), sse_clients_.end(), &sink),
                            sse_clients_.end()
                        );
                    }
                    return false;
                }
            );
        });
    }

public:
    http_api_server(int port,
                    process_tree_manager& mgr,
                    config_manager& cm,
                    asio::strand<asio::io_context::executor_type>& strand,
                    const std::string& static_dir = "")
        : port_(port), static_dir_(static_dir)
        , mgr_(mgr), config_manager_(cm), strand_(strand)
    {
        setup_routes();
    }

    void set_static_dir(std::string_view dir) { static_dir_ = dir; }
    void set_port_tracker(PortTracker* pt) { port_tracker_ = pt; }
    void set_udp_port_tracker(UdpPortTracker* pt) { udp_port_tracker_ = pt; }
    void set_icon_cache(icon_cache* ic) { icon_cache_ = ic; }
    void set_on_config_change(std::function<void(const ConfigV2&)> cb) { on_config_change_ = std::move(cb); }

    // Broadcast an SSE event to all connected clients.
    // Thread-safe: uses sse_mutex_.
    void broadcast_event(const std::string& event, const json& data) {
        std::scoped_lock lock(sse_mutex_);
        std::string message = "event: " + event + "\ndata: " + data.dump() + "\n\n";
        for (auto* sink : sse_clients_) {
            if (sink->is_writable()) {
                sink->write(message.c_str(), message.size());
            }
        }
    }

    // Convenience: broadcast process exit (called from strand on ETW ProcessStop)
    void broadcast_process_exit(DWORD pid) {
        json j;
        j["pid"] = pid;
        j["hijacked"] = false;
        j["exited"] = true;
        broadcast_event("process_exit", j);
    }

    // Called from process_tree_manager's on_tree_changed callback (runs on strand).
    // Publishes atomic snapshot and fires SSE.
    void on_tree_changed() {
        auto t0 = std::chrono::steady_clock::now();
        publish_tree_snapshot();
        auto t1 = std::chrono::steady_clock::now();
        broadcast_event("process_update", json{{"type", "tree_changed"}});
        auto t2 = std::chrono::steady_clock::now();
        PC_LOG_DEBUG("[Latency] snapshot={}us sse={}us",
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count());
    }

    bool start() {
        if (running_) return true;
        running_ = true;
        server_thread_ = std::jthread([this]() {
            PC_LOG_INFO("HTTP API server starting on port {}", port_);
            if (!server_.listen("127.0.0.1", port_)) {
                PC_LOG_ERROR("Failed to start HTTP server on port {}", port_);
                running_ = false;
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return running_;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        server_.stop();
        if (server_thread_.joinable()) server_thread_.join();
        PC_LOG_INFO("HTTP API server stopped");
    }

    int get_port() const { return port_; }
};

} // namespace clew
