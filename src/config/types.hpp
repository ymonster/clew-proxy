#pragma once

#include <string>
#include <format>
#include <vector>
#include <cstdint>
#include <unordered_set>
#include <chrono>
#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>


namespace clew {

// ============================================================
// CIDR Range - IP range matching
// ============================================================

struct CidrRange {
    uint32_t network = 0;   // Host byte order
    uint32_t mask = 0;

    static uint32_t ip_to_uint(const std::string& ip) {
        uint32_t a;
        uint32_t b;
        uint32_t c;
        uint32_t d;
        if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
        return (a << 24) | (b << 16) | (c << 8) | d;
    }

    static std::string uint_to_ip(uint32_t ip) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                 (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        return buf;
    }

    // Parse "10.0.0.0/8" or "1.2.3.4"
    static CidrRange parse(const std::string& cidr) {
        CidrRange r;
        size_t slash = cidr.find('/');
        if (slash == std::string::npos) {
            r.network = ip_to_uint(cidr);
            r.mask = 0xFFFFFFFF;
        } else {
            int prefix_len = std::stoi(cidr.substr(slash + 1));
            r.mask = (prefix_len == 0) ? 0 : (0xFFFFFFFF << (32 - prefix_len));
            r.network = ip_to_uint(cidr.substr(0, slash)) & r.mask;
        }
        return r;
    }

    bool matches(uint32_t ip) const {
        return (ip & mask) == network;
    }

    bool matches(const std::string& ip) const {
        return matches(ip_to_uint(ip));
    }

    std::string to_string() const {
        // Find prefix length from mask
        int prefix = 0;
        uint32_t m = mask;
        while (m & 0x80000000) { prefix++; m <<= 1; }
        return std::format("{}/{}", uint_to_ip(network), prefix);
    }
};

inline void to_json(nlohmann::json& j, const CidrRange& r) {
    j = r.to_string();
}

inline void from_json(const nlohmann::json& j, CidrRange& r) {
    r = CidrRange::parse(j.get<std::string>());
}

// ============================================================
// Port Range
// ============================================================

struct PortRange {
    uint16_t start = 0;
    uint16_t end = 0;

    // Parse "80", "80-443", "1024-65535"
    static PortRange parse(const std::string& s) {
        PortRange r;
        size_t dash = s.find('-');
        if (dash == std::string::npos) {
            r.start = r.end = static_cast<uint16_t>(std::stoi(s));
        } else {
            r.start = static_cast<uint16_t>(std::stoi(s.substr(0, dash)));
            r.end = static_cast<uint16_t>(std::stoi(s.substr(dash + 1)));
        }
        return r;
    }

    bool matches(uint16_t port) const {
        return port >= start && port <= end;
    }

    std::string to_string() const {
        if (start == end) return std::to_string(start);
        return std::format("{}-{}", start, end);
    }
};

inline void to_json(nlohmann::json& j, const PortRange& r) {
    j = r.to_string();
}

inline void from_json(const nlohmann::json& j, PortRange& r) {
    r = PortRange::parse(j.get<std::string>());
}

// ============================================================
// Proxy Target - Per-rule proxy destination
// ============================================================

struct ProxyTarget {
    std::string type = "socks5";   // "socks5" | "http"
    std::string host = "127.0.0.1";
    uint16_t port = 7890;
    std::string user;              // Optional auth
    std::string password;          // Optional auth

    bool operator==(const ProxyTarget& o) const {
        return type == o.type && host == o.host && port == o.port;
    }

    std::string to_string() const {
        return std::format("{}://{}:{}", type, host, port);
    }
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProxyTarget, type, host, port, user, password)

// ============================================================
// Proxy Group - Named proxy configuration
// ============================================================

struct ProxyGroup {
    uint32_t id = 0;                    // Auto-increment ID, 0 = default group
    std::string name = "default";       // Display name
    std::string host = "127.0.0.1";
    uint16_t port = 7890;
    std::string type = "socks5";        // Only "socks5" for now
    std::string test_url = "http://www.gstatic.com/generate_204";

    std::string to_string() const {
        return std::format("{}://{}:{}", type, host, port);
    }
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProxyGroup, id, name, host, port, type, test_url)

// ============================================================
// Traffic Filter - IP/port include/exclude
// ============================================================

struct TrafficFilter {
    std::vector<CidrRange> include_cidrs;   // If non-empty, only proxy matching
    std::vector<CidrRange> exclude_cidrs;   // Always exclude matching
    std::vector<PortRange> include_ports;   // If non-empty, only proxy matching
    std::vector<PortRange> exclude_ports;   // Always exclude matching
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TrafficFilter, include_cidrs, exclude_cidrs, include_ports, exclude_ports)

// ============================================================
// Auto Rule
// ============================================================

struct AutoRule {
    AutoRule() = default;
    AutoRule(const AutoRule&) = default;
    AutoRule& operator=(const AutoRule&) = default;
    AutoRule(AutoRule&&) noexcept = default;
    AutoRule& operator=(AutoRule&&) noexcept = default;

    std::string id;
    std::string name;
    bool enabled = true;
    std::string process_name;       // Wildcard pattern (e.g. "curl*")
    std::string cmdline_pattern;    // Wildcard pattern (optional)
    std::string image_path_pattern; // Directory prefix match (e.g. "C:\\Python311\\")
    bool hack_tree = false;
    TrafficFilter dst_filter;
    uint32_t proxy_group_id = 0;    // References ProxyGroup, 0 = default group
    std::string protocol = "tcp";   // "tcp", "udp", or "both"
    ProxyTarget proxy;              // Legacy: kept for backward compat during migration

    // Runtime helpers
    bool matches_tcp() const { return protocol == "tcp" || protocol == "both"; }
    bool matches_udp() const { return protocol == "udp" || protocol == "both"; }

    // Runtime state (not serialized)
    std::unordered_set<DWORD> excluded_pids;
    std::unordered_set<DWORD> matched_pids;
};

inline void to_json(nlohmann::json& j, const AutoRule& r) {
    j = nlohmann::json{
        {"id", r.id},
        {"name", r.name},
        {"enabled", r.enabled},
        {"process_name", r.process_name},
        {"cmdline_pattern", r.cmdline_pattern},
        {"image_path_pattern", r.image_path_pattern},
        {"hack_tree", r.hack_tree},
        {"dst_filter", r.dst_filter},
        {"proxy_group_id", r.proxy_group_id},
        {"protocol", r.protocol},
        {"proxy", r.proxy}
    };
}

inline void from_json(const nlohmann::json& j, AutoRule& r) {
    r.id = j.value("id", std::string(""));
    r.name = j.value("name", "");
    r.enabled = j.value("enabled", true);
    r.process_name = j.value("process_name", "");
    r.cmdline_pattern = j.value("cmdline_pattern", "");
    r.image_path_pattern = j.value("image_path_pattern", "");
    r.hack_tree = j.value("hack_tree", true);
    if (j.contains("dst_filter")) j.at("dst_filter").get_to(r.dst_filter);
    r.proxy_group_id = j.value("proxy_group_id", 0u);
    r.protocol = j.value("protocol", std::string("tcp"));
    if (j.contains("proxy")) j.at("proxy").get_to(r.proxy);
    // Runtime state not deserialized
}

// ============================================================
// UI Config
// ============================================================

struct UiConfig {
    int window_width = 1200;
    int window_height = 800;
    int window_x = -1;      // -1 = center
    int window_y = -1;
    bool dark_mode = true;
    bool close_to_tray = false;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(UiConfig, window_width, window_height, window_x, window_y, dark_mode, close_to_tray)

// ============================================================
// Config V2 - Top-level configuration
// ============================================================

struct DnsConfig {
    bool enabled = false;                       // Master DNS proxy switch
    std::string mode = "forwarder";             // "forwarder" (future: "hook")
    std::string upstream_host = "8.8.8.8";      // Upstream DNS server
    uint16_t upstream_port = 53;                // Upstream DNS port (fixed to 53 for now)
    std::string listen_host = "127.0.0.2";      // Local listen address (forwarder mode)
    uint16_t listen_port = 53;                  // Local listen port (forwarder mode)
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DnsConfig, enabled, mode, upstream_host, upstream_port, listen_host, listen_port)

struct ApiAuth {
    bool enabled = false;       // Off by default
    std::string token = "";     // Bearer token; must be non-empty when enabled
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ApiAuth, enabled, token)

struct ConfigV2 {
    int version = 2;
    ProxyTarget default_proxy;
    std::vector<ProxyGroup> proxy_groups;       // NEW: named proxy configurations
    uint32_t next_group_id = 1;                 // NEW: auto-increment counter (0 = default, starts at 1)
    std::vector<std::string> default_exclude_cidrs = {
        "127.0.0.0/8",
        "10.0.0.0/8",
        "172.16.0.0/12",
        "192.168.0.0/16",
        "169.254.0.0/16"
    };
    std::vector<AutoRule> auto_rules;
    UiConfig ui;
    int io_threads = 0;                         // NEW: 0 = hardware_concurrency()/2
    std::string log_level = "info";             // Runtime log level: debug/info/warning/error
    DnsConfig dns;                              // NEW: DNS proxy configuration
    ApiAuth auth;                               // NEW: API token authentication (default off)
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ConfigV2, version, default_proxy, proxy_groups, next_group_id, default_exclude_cidrs, auto_rules, ui, io_threads, log_level, dns, auth)

} // namespace clew
