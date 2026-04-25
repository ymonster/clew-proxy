#pragma once

// UDP Session Table: maps app src_port -> original destination + metadata.
// Written by UDP NETWORK workers (on intercept), read by UDP Relay.
//
// Synchronization: shared_mutex (NETWORK workers = exclusive write, relay = shared read).
// Low contention: writes only on new src_port first seen, reads on every relay packet.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <vector>

namespace clew {

struct UdpSession {
    uint32_t orig_dst_addr{};   // network byte order (raw from IP header)
    uint16_t orig_dst_port{};   // network byte order (raw from UDP header)
    uint32_t app_src_addr{};    // network byte order
    uint16_t app_src_port{};    // host byte order (key)
    uint32_t pid{};
    uint32_t group_id{};
    UINT32 ifidx{};
    UINT32 subifidx{};
    std::chrono::steady_clock::time_point last_active{std::chrono::steady_clock::now()};
};

class UdpSessionTable {
public:
    // Insert or update session (called from NETWORK workers)
    void upsert(uint16_t app_port, const UdpSession& session) {
        std::unique_lock lk(mu_);
        sessions_[app_port] = session;
    }

    // Lookup session (called from relay)
    std::optional<UdpSession> lookup(uint16_t app_port) const {
        std::shared_lock lk(mu_);
        auto it = sessions_.find(app_port);
        if (it != sessions_.end())
            return it->second;
        return std::nullopt;
    }

    // Update last_active timestamp (called from relay on upstream send)
    void touch(uint16_t app_port) {
        std::unique_lock lk(mu_);
        auto it = sessions_.find(app_port);
        if (it != sessions_.end())
            it->second.last_active = std::chrono::steady_clock::now();
    }

    // Remove session
    void remove(uint16_t app_port) {
        std::unique_lock lk(mu_);
        sessions_.erase(app_port);
    }

    // Remove expired sessions. Returns number removed.
    size_t cleanup_expired(std::chrono::seconds timeout = std::chrono::seconds(120)) {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock lk(mu_);
        // DNS (port 53) gets shorter timeout
        return std::erase_if(sessions_, [now, timeout](const auto& kv) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - kv.second.last_active);
            auto effective_timeout = (kv.second.orig_dst_port == htons(53))
                                    ? std::chrono::seconds(10)
                                    : timeout;
            return age > effective_timeout;
        });
    }

    // Get all active sessions (for API)
    std::vector<UdpSession> snapshot() const {
        std::shared_lock lk(mu_);
        std::vector<UdpSession> result;
        result.reserve(sessions_.size());
        for (const auto& [_, s] : sessions_)
            result.push_back(s);
        return result;
    }

    size_t size() const {
        std::shared_lock lk(mu_);
        return sessions_.size();
    }

private:
    mutable std::shared_mutex mu_;
    std::unordered_map<uint16_t, UdpSession> sessions_;
};

} // namespace clew
