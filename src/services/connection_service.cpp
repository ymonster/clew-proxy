#include "services/connection_service.hpp"

#include <format>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "domain/process_tree_manager.hpp"
#include "process/flat_tree.hpp"
#include "process/tcp_table.hpp"
#include "process/udp_table.hpp"
#include "rules/rule_engine_v3.hpp"
#include "rules/traffic_filter.hpp"
#include "udp/udp_port_tracker.hpp"

namespace clew {

namespace {

std::string proxy_status_from_filter(const rule_engine_v3& rules,
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

} // namespace

connection_service::connection_service(strand_bound_manager& exec, UdpPortTracker* udp_tracker)
    : exec_(exec), udp_tracker_(udp_tracker) {}

nlohmann::json connection_service::list_tcp(std::optional<std::uint32_t> pid_filter) const {
    DWORD filter_pid = pid_filter ? static_cast<DWORD>(*pid_filter) : 0;
    auto connections = tcp_table::get_connections(filter_pid);

    return exec_.query([&connections, filter_pid](const domain::process_tree_manager& m) -> nlohmann::json {
        nlohmann::json arr = nlohmann::json::array();
        const auto& tree  = m.tree();
        const auto& rules = m.rules();

        for (const auto& conn : connections) {
            if (filter_pid == 0 &&
                (conn.state == "LISTEN" ||
                 (conn.remote_ip == "0.0.0.0" && conn.remote_port == 0))) {
                continue;
            }

            nlohmann::json c;
            c["pid"]         = conn.pid;
            c["local_ip"]    = conn.local_ip;
            c["local_port"]  = conn.local_port;
            c["remote_ip"]   = conn.remote_ip;
            c["remote_port"] = conn.remote_port;
            c["state"]       = conn.state;
            c["dest"]        = std::format("{}:{}", conn.remote_ip, conn.remote_port);

            uint32_t idx = tree.find_by_pid(conn.pid);
            bool pid_alive   = (idx != INVALID_IDX);
            bool is_hijacked = pid_alive && tree.at(idx).is_proxied();
            c["hijacked"]  = is_hijacked;
            c["pid_alive"] = pid_alive;

            if (!is_hijacked) {
                c["proxy_status"] = "-";
            } else if (conn.state == "LISTEN" ||
                       (conn.remote_ip == "0.0.0.0" && conn.remote_port == 0)) {
                c["proxy_status"] = "LISTEN";
            } else {
                c["proxy_status"] = proxy_status_from_filter(rules, tree, conn.pid,
                                                              conn.remote_ip, conn.remote_port);
            }

            c["process_name"] = (idx != INVALID_IDX)
                ? std::string(tree.at(idx).name_u8)
                : std::string{"unknown"};

            arr.push_back(std::move(c));
        }
        return arr;
    });
}

nlohmann::json connection_service::list_udp(std::optional<std::uint32_t> pid_filter) const {
    DWORD filter_pid = pid_filter ? static_cast<DWORD>(*pid_filter) : 0;
    auto endpoints = udp_table::get_endpoints(filter_pid);
    UdpPortTracker* tracker = udp_tracker_;

    return exec_.query([&endpoints, filter_pid, tracker](const domain::process_tree_manager& m) -> nlohmann::json {
        nlohmann::json arr = nlohmann::json::array();
        const auto& tree  = m.tree();
        const auto& rules = m.rules();

        for (const auto& ep : endpoints) {
            if (ep.local_port == 0 && filter_pid == 0) continue;

            nlohmann::json c;
            c["pid"]        = ep.pid;
            c["local_ip"]   = ep.local_ip;
            c["local_port"] = ep.local_port;
            c["state"]      = "BOUND";

            std::string remote_ip;
            uint16_t    remote_port = 0;
            if (tracker && tracker->is_active(ep.local_port)) {
                const auto& entry = tracker->peek(ep.local_port);
                struct in_addr addr;
                addr.s_addr = htonl(entry.remote_addr[0]);
                char buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &addr, buf, sizeof(buf));
                remote_ip   = buf;
                remote_port = entry.remote_port;
            }
            c["remote_ip"]   = remote_ip;
            c["remote_port"] = remote_port;
            c["dest"]        = remote_ip.empty() ? std::string{} : std::format("{}:{}", remote_ip, remote_port);

            uint32_t idx = tree.find_by_pid(ep.pid);
            bool pid_alive   = (idx != INVALID_IDX);
            bool is_hijacked = pid_alive && tree.at(idx).is_proxied();
            c["hijacked"]  = is_hijacked;
            c["pid_alive"] = pid_alive;

            if (!is_hijacked) {
                c["proxy_status"] = "-";
            } else {
                c["proxy_status"] = proxy_status_from_filter(rules, tree, ep.pid, remote_ip, remote_port);
            }

            c["process_name"] = (idx != INVALID_IDX)
                ? std::string(tree.at(idx).name_u8)
                : std::string{"unknown"};

            arr.push_back(std::move(c));
        }
        return arr;
    });
}

} // namespace clew
