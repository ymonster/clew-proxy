#pragma once

// connection_service — OS-level TCP/UDP connection table enriched with
// process tree + hijack status.

#include <cstdint>
#include <optional>

#include <nlohmann/json.hpp>

#include "domain/strand_bound.hpp"

namespace clew {

class UdpPortTracker;

class connection_service {
public:
    connection_service(strand_bound_manager& exec, UdpPortTracker* udp_tracker);

    connection_service(const connection_service&)            = delete;
    connection_service& operator=(const connection_service&) = delete;

    // GET /api/tcp?pid=X — TCP table with hijack status.
    [[nodiscard]] nlohmann::json list_tcp(std::optional<std::uint32_t> pid_filter) const;

    // GET /api/udp?pid=X — UDP table enriched with UdpPortTracker remote info.
    [[nodiscard]] nlohmann::json list_udp(std::optional<std::uint32_t> pid_filter) const;

private:
    strand_bound_manager& exec_;
    UdpPortTracker*       udp_tracker_;
};

} // namespace clew
