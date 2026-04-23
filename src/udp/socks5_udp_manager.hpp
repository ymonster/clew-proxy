#pragma once

// SOCKS5 UDP ASSOCIATE session manager — per-local-port sessions.
//
// Each hijacked UDP socket (identified by app_port) gets its own
// SOCKS5 UDP ASSOCIATE session, per RFC 1928 per-client semantics.
// This eliminates cross-process response correlation and misdelivery risks.
//
// Sessions are lazily created on first upstream packet and cleaned up
// when the port is no longer active.

#define ASIO_STANDALONE
#include <asio.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <functional>
#include "core/log.hpp"

#include "udp/socks5_udp_session.hpp"

namespace clew {

struct UdpProxyGroupConfig {
    std::string host;
    uint16_t port{1080};
};

// Callback when a new per-port session is created (for spawning downstream coroutine)
using SessionCreatedCallback = std::function<void(uint16_t app_port,
                                                   std::shared_ptr<Socks5UdpSession>)>;

class Socks5UdpManager {
public:
    Socks5UdpManager(asio::io_context& ioc,
                     const std::unordered_map<uint32_t, UdpProxyGroupConfig>& groups)
        : ioc_(ioc)
        , groups_(groups)
    {}

    ~Socks5UdpManager() { stop(); }

    void set_on_session_created(SessionCreatedCallback cb) {
        std::lock_guard lk(mu_);
        on_session_created_ = std::move(cb);
    }

    // Get or create session for an app_port. group_id used to look up proxy config.
    std::shared_ptr<Socks5UdpSession> get_or_create(uint16_t app_port, uint32_t group_id) {
        std::unique_lock lk(mu_);

        if (stopped_) return nullptr;

        auto it = sessions_.find(app_port);
        if (it != sessions_.end() && it->second && it->second->is_alive()) {
            return it->second;
        }

        auto git = groups_.find(group_id);
        if (git == groups_.end()) {
            PC_LOG_WARN("[SOCKS5-MGR] Unknown group_id={} for port={}", group_id, app_port);
            return nullptr;
        }

        auto session = std::make_shared<Socks5UdpSession>(
            ioc_, git->second.host, git->second.port);

        if (!session->establish()) {
            PC_LOG_ERROR("[SOCKS5-MGR] Failed to establish session for port={} group={}",
                          app_port, group_id);
            return nullptr;
        }

        sessions_[app_port] = session;
        PC_LOG_INFO("[SOCKS5-MGR] Session created for port={} group={} (total={})",
                     app_port, group_id, sessions_.size());

        // Notify relay to spawn downstream coroutine — copy callback under lock, invoke outside
        auto cb = on_session_created_;
        if (cb) {
            // Release lock before callback to avoid deadlock; unique_lock dtor is a no-op if !owns_lock
            lk.unlock();
            cb(app_port, session);
        }

        return session;
    }

    // Remove session for a specific port (e.g., on socket close)
    void remove_session(uint16_t app_port) {
        std::lock_guard lk(mu_);
        auto it = sessions_.find(app_port);
        if (it != sessions_.end()) {
            if (it->second) it->second->close();
            sessions_.erase(it);
            PC_LOG_DEBUG("[SOCKS5-MGR] Session removed for port={}", app_port);
        }
    }

    void stop() {
        if (stopped_) return;
        stopped_ = true;

        std::lock_guard lk(mu_);
        on_session_created_ = nullptr;
        for (auto& [_, session] : sessions_) {
            if (session) session->close();
        }
        sessions_.clear();
        PC_LOG_INFO("[SOCKS5-MGR] Stopped, all sessions closed");
    }

    size_t active_count() const {
        std::lock_guard lk(mu_);
        size_t count = 0;
        for (const auto& [_, s] : sessions_)
            if (s && s->is_alive()) count++;
        return count;
    }

private:
    asio::io_context& ioc_;
    const std::unordered_map<uint32_t, UdpProxyGroupConfig>& groups_;

    std::atomic<bool> stopped_{false};
    mutable std::mutex mu_;
    std::unordered_map<uint16_t, std::shared_ptr<Socks5UdpSession>> sessions_;  // key: app_port
    SessionCreatedCallback on_session_created_;
};

} // namespace clew
