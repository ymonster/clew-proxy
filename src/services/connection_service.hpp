#pragma once

// connection_service — OS-level TCP/UDP connection table enriched with
// process tree + hijack status.

#include <cstdint>
#include <optional>

#include <nlohmann/json.hpp>

#include "domain/strand_bound.hpp"

namespace clew {

class UdpPortTracker;
class UdpSessionTable;

class connection_service {
public:
    connection_service(strand_bound_manager& exec, UdpPortTracker* udp_tracker,
                       const UdpSessionTable* udp_sessions);

    connection_service(const connection_service&)            = delete;
    connection_service& operator=(const connection_service&) = delete;

    // GET /api/tcp?pid=X — TCP table with hijack status.
    [[nodiscard]] nlohmann::json list_tcp(std::optional<std::uint32_t> pid_filter) const;

    // GET /api/udp?pid=X — UDP table enriched with hijack status and, for ports
    // with live proxied traffic, the destination recorded in UdpSessionTable.
    [[nodiscard]] nlohmann::json list_udp(std::optional<std::uint32_t> pid_filter) const;

private:
    strand_bound_manager&  exec_;
    UdpPortTracker*        udp_tracker_;
    const UdpSessionTable* udp_sessions_;
};

} // namespace clew
