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
#include "udp/udp_session_table.hpp"

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

nlohmann::json tcp_entry_to_json(const tcp_connection& conn,
                                  const flat_tree& tree,
                                  const rule_engine_v3& rules) {
    nlohmann::json c;
    c["pid"]         = conn.pid;
    c["local_ip"]    = conn.local_ip;
    c["local_port"]  = conn.local_port;
    c["remote_ip"]   = conn.remote_ip;
    c["remote_port"] = conn.remote_port;
    c["state"]       = conn.state;
    c["dest"]        = std::format("{}:{}", conn.remote_ip, conn.remote_port);

    uint32_t idx     = tree.find_by_pid(conn.pid);
    bool pid_alive   = (idx != INVALID_IDX);
    bool is_hijacked = pid_alive && tree.at(idx).is_proxied();
    c["hijacked"]    = is_hijacked;
    c["pid_alive"]   = pid_alive;

    if (!is_hijacked) {
        c["proxy_status"] = "-";
    } else if (conn.state == "LISTEN" ||
               (conn.remote_ip == "0.0.0.0" && conn.remote_port == 0)) {
        c["proxy_status"] = "LISTEN";
    } else {
        c["proxy_status"] = proxy_status_from_filter(rules, tree, conn.pid,
                                                      conn.remote_ip, conn.remote_port);
    }

    c["process_name"] = pid_alive ? std::string(tree.at(idx).name_u8) : std::string{"unknown"};
    return c;
}

nlohmann::json udp_entry_to_json(const udp_endpoint& ep,
                                  const flat_tree& tree,
                                  const rule_engine_v3& rules,
                                  const UdpPortTracker* tracker,
                                  const UdpSessionTable* sessions) {
    nlohmann::json c;
    c["pid"]        = ep.pid;
    c["local_ip"]   = ep.local_ip;
    c["local_port"] = ep.local_port;
    c["state"]      = "BOUND";

    // Destination comes from UdpSessionTable, which the NETWORK layer updates
    // per packet. A UDP socket can sendto() many destinations, so this reports
    // the one actually in use rather than a connect()-time address, and it also
    // covers unconnected sockets. Empty until the port sends proxied traffic.
    std::string remote_ip;
    uint16_t    remote_port = 0;
    if (sessions && tracker && tracker->is_active(ep.local_port)) {
        if (auto session = sessions->lookup(ep.local_port)) {
            struct in_addr addr;
            addr.s_addr = session->orig_dst_addr;  // already network byte order
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr, buf, sizeof(buf));
            remote_ip   = buf;
            remote_port = ntohs(session->orig_dst_port);
        }
    }
    c["remote_ip"]   = remote_ip;
    c["remote_port"] = remote_port;
    c["dest"]        = remote_ip.empty() ? std::string{} : std::format("{}:{}", remote_ip, remote_port);

    uint32_t idx     = tree.find_by_pid(ep.pid);
    bool pid_alive   = (idx != INVALID_IDX);
    bool is_hijacked = pid_alive && tree.at(idx).is_proxied();
    c["hijacked"]    = is_hijacked;
    c["pid_alive"]   = pid_alive;

    if (!is_hijacked) {
        c["proxy_status"] = "-";
    } else {
        c["proxy_status"] = proxy_status_from_filter(rules, tree, ep.pid, remote_ip, remote_port);
    }

    c["process_name"] = pid_alive ? std::string(tree.at(idx).name_u8) : std::string{"unknown"};
    return c;
}

} // namespace

connection_service::connection_service(strand_bound_manager& exec, UdpPortTracker* udp_tracker,
                                       const UdpSessionTable* udp_sessions)
    : exec_(exec), udp_tracker_(udp_tracker), udp_sessions_(udp_sessions) {}

nlohmann::json connection_service::list_tcp(std::optional<std::uint32_t> pid_filter) const {
    // NO_PID_FILTER (= UINT_MAX) is the "no filter, return all" sentinel.
    // We can't reuse 0 because PID 0 (Idle) is a real selectable process —
    // collapsing nullopt and optional(0) onto 0 would make selecting Idle
    // dump the whole OS connection table.
    DWORD filter_pid = pid_filter ? static_cast<DWORD>(*pid_filter) : NO_PID_FILTER;
    auto connections = tcp_table::get_connections(filter_pid);

    return exec_.query([&connections, filter_pid](const domain::process_tree_manager& m) -> nlohmann::json {
        nlohmann::json arr = nlohmann::json::array();
        const auto& tree  = m.tree();
        const auto& rules = m.rules();

        for (const auto& conn : connections) {
            if (filter_pid == NO_PID_FILTER &&
                (conn.state == "LISTEN" ||
                 (conn.remote_ip == "0.0.0.0" && conn.remote_port == 0))) {
                continue;
            }
            arr.push_back(tcp_entry_to_json(conn, tree, rules));
        }
        return arr;
    });
}

nlohmann::json connection_service::list_udp(std::optional<std::uint32_t> pid_filter) const {
    DWORD filter_pid = pid_filter ? static_cast<DWORD>(*pid_filter) : NO_PID_FILTER;
    auto endpoints = udp_table::get_endpoints(filter_pid);
    const UdpPortTracker*  tracker  = udp_tracker_;
    const UdpSessionTable* sessions = udp_sessions_;

    return exec_.query([&endpoints, filter_pid, tracker, sessions](const domain::process_tree_manager& m) -> nlohmann::json {
        nlohmann::json arr = nlohmann::json::array();
        const auto& tree  = m.tree();
        const auto& rules = m.rules();

        for (const auto& ep : endpoints) {
            if (ep.local_port == 0 && filter_pid == NO_PID_FILTER) continue;
            arr.push_back(udp_entry_to_json(ep, tree, rules, tracker, sessions));
        }
        return arr;
    });
}

} // namespace clew
