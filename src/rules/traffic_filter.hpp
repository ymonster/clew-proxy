#pragma once

#include <cstdint>
#include "config/types.hpp"

namespace clew {

// ============================================================
// Traffic Filter Engine
// Determines whether a connection should be proxied based on
// destination IP/port and the rule's TrafficFilter config.
// ============================================================

class TrafficFilterEngine {
public:
    // Returns true if the connection should be proxied.
    // Evaluation order:
    //   1. Loopback (127.0.0.0/8) → always DIRECT (never proxy)
    //   2. Exclude CIDRs → DIRECT
    //   3. Exclude Ports → DIRECT
    //   4. Include CIDRs (if non-empty, must match) → proxy or DIRECT
    //   5. Include Ports (if non-empty, must match) → proxy or DIRECT
    //   6. Default: proxy
    static bool should_proxy(uint32_t dest_ip, uint16_t dest_port, const TrafficFilter& filter) {
        // Always skip loopback
        if ((dest_ip >> 24) == 127) return false;

        // Check exclude CIDRs
        for (const auto& cidr : filter.exclude_cidrs) {
            if (cidr.matches(dest_ip)) return false;
        }

        // Check exclude ports
        for (const auto& pr : filter.exclude_ports) {
            if (pr.matches(dest_port)) return false;
        }

        // Check include CIDRs (whitelist: if non-empty, must match at least one)
        if (!filter.include_cidrs.empty()) {
            bool cidr_match = false;
            for (const auto& cidr : filter.include_cidrs) {
                if (cidr.matches(dest_ip)) { cidr_match = true; break; }
            }
            if (!cidr_match) return false;
        }

        // Check include ports (whitelist: if non-empty, must match at least one)
        if (!filter.include_ports.empty()) {
            bool port_match = false;
            for (const auto& pr : filter.include_ports) {
                if (pr.matches(dest_port)) { port_match = true; break; }
            }
            if (!port_match) return false;
        }

        return true;
    }

    // Convenience overload with string IP
    static bool should_proxy(const std::string& dest_ip, uint16_t dest_port, const TrafficFilter& filter) {
        return should_proxy(CidrRange::ip_to_uint(dest_ip), dest_port, filter);
    }
};

} // namespace clew
