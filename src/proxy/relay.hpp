#pragma once

// Bidirectional relay without parallel_group. Either direction ending closes
// the whole session, preserving the previous || business policy.

#define ASIO_STANDALONE
#include <asio.hpp>
#include <asio/use_awaitable.hpp>

#include <array>
#include <atomic>
#include <format>
#include "core/log.hpp"

#include "core/port_tracker.hpp"
#include "proxy/socks5_async.hpp"

inline std::string ip_host_to_string(uint32_t ip) {
    return std::format("{}.{}.{}.{}",
                       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                       (ip >> 8) & 0xFF,  ip & 0xFF);
}

namespace clew {

using asio::ip::tcp;

struct ProxyGroupConfig {
    std::string host;
    uint16_t    port{7890};
};

// Retained for the local echo peer in test_relay.cpp.
inline asio::awaitable<void>
pipe(tcp::socket& from, tcp::socket& to)
{
    std::array<char, 32 * 1024> buf{};
    try {
        for (;;) {
            auto n = co_await from.async_read_some(
                asio::buffer(buf), asio::use_awaitable);
            co_await asio::async_write(
                to, asio::buffer(buf, n), asio::use_awaitable);
        }
    } catch (...) {
        std::error_code ec;
        to.shutdown(tcp::socket::shutdown_send, ec);
    }
}

inline std::atomic<int> g_relay_sessions_alive{0};
struct RelaySessionAlive {
    RelaySessionAlive() {
        g_relay_sessions_alive.fetch_add(1, std::memory_order_relaxed);
    }
    ~RelaySessionAlive() {
        g_relay_sessions_alive.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct RelaySession {
    tcp::socket client_sock;
    tcp::socket proxy_sock;
    PortTracker *tracker;
    asio::strand<asio::io_context::executor_type> *strand;
    asio::io_context *ioc;
    uint16_t src_port;
    int64_t flow_ts;
    std::string dest_ip_str;
    uint16_t dest_port;
    std::atomic<bool> closing{false};
    std::atomic<int> remaining{2};
    RelaySessionAlive alive;
};

inline void finish_relay_session(RelaySession *session) {
    if (!session->closing.exchange(true, std::memory_order_acq_rel)) {
        // The sockets stay alive until both direction coroutines finish.
        // Native shutdown wakes their pending Winsock operations without
        // racing a second close or destroying Asio's socket state early.
        (void)::shutdown(session->client_sock.native_handle(), SD_BOTH);
        (void)::shutdown(session->proxy_sock.native_handle(), SD_BOTH);
    }

    if (session->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (!session->ioc->stopped()) {
            auto *tracker = session->tracker;
            auto *strand = session->strand;
            const uint16_t src_port = session->src_port;
            const int64_t flow_ts = session->flow_ts;
            asio::post(*strand, [tracker, src_port, flow_ts] {
                tracker->clear_if(src_port, flow_ts);
            });
            PC_LOG_DEBUG("[RELAY] Closed port={} ({}:{})", session->src_port,
                         session->dest_ip_str, session->dest_port);
        }
        delete session;
    }
}

struct RelayFinishGuard {
    RelaySession *session;
    ~RelayFinishGuard() { finish_relay_session(session); }
};

inline asio::awaitable<void> relay_direction(RelaySession *session,
                                             bool client_to_proxy) {
    RelayFinishGuard finish_guard{session};
    auto &from = client_to_proxy ? session->client_sock : session->proxy_sock;
    auto &to = client_to_proxy ? session->proxy_sock : session->client_sock;
    std::array<char, 32 * 1024> buf{};

    try {
        for (;;) {
            auto n = co_await from.async_read_some(asio::buffer(buf),
                                                   asio::use_awaitable);
            co_await asio::async_write(to, asio::buffer(buf, n),
                                       asio::use_awaitable);
        }
    } catch (...) {
        // EOF, reset, and other errors all terminate the whole session in
        // this implementation. Error-fidelity propagation is deliberately
        // out of scope.
    }
}

inline asio::awaitable<void> handle_connection(
    tcp::socket client_sock, PortTracker &tracker,
    asio::strand<asio::io_context::executor_type> &strand,
    const std::unordered_map<uint32_t, ProxyGroupConfig> &groups) {
    auto ex = co_await asio::this_coro::executor;

    std::error_code ep_ec;
    auto ep = client_sock.remote_endpoint(ep_ec);
    if (ep_ec)
        co_return;
    const uint16_t src_port = ep.port();

    auto taken = tracker.take(src_port);
    if (!taken) {
        PC_LOG_DEBUG("[RELAY] No tracker entry for port {}", src_port);
        co_return;
    }
    const TrackerEntry *entry = &taken->entry;
    const int64_t flow_ts = taken->connect_ts;

    auto release_slot = [&tracker, &strand, src_port, flow_ts]() {
        asio::post(strand, [&tracker, src_port, flow_ts]() {
            tracker.clear_if(src_port, flow_ts);
        });
    };

    struct GroupInfo {
        std::string host;
        uint16_t port;
    };

    auto cfg_opt = co_await asio::co_spawn(
        strand,
        [&groups, group_id = entry->group_id]()
            -> asio::awaitable<std::optional<GroupInfo>> {
            auto it = groups.find(group_id);
            if (it == groups.end())
                co_return std::nullopt;
            co_return GroupInfo{it->second.host, it->second.port};
        },
        asio::use_awaitable);

    if (!cfg_opt) {
        PC_LOG_WARN("[RELAY] Group {} not found for port {}", entry->group_id,
                    src_port);
        release_slot();
        co_return;
    }

    auto dest_addr = entry->remote_addr[0];
    auto dest_port = entry->remote_port;

    if (dest_port == 53) {
        auto orig = dest_addr;
        dest_addr = 0x08080808;
        PC_LOG_DEBUG("[RELAY] DNS redirect port={} {}:53 -> 8.8.8.8:53",
                     src_port, ip_host_to_string(orig));
    }

    auto dest_ip_str = ip_host_to_string(dest_addr);
    RelaySession *session = nullptr;
    bool handed_off = false;

    try {
        auto t0 = std::chrono::steady_clock::now();

        tcp::resolver resolver{ex};
        auto eps = co_await resolver.async_resolve(
            cfg_opt->host, std::to_string(cfg_opt->port), asio::use_awaitable);

        tcp::socket proxy_sock{ex};
        co_await asio::async_connect(proxy_sock, eps, asio::use_awaitable);

        auto t1 = std::chrono::steady_clock::now();
        co_await socks5_handshake(proxy_sock, dest_addr, dest_port);
        auto t2 = std::chrono::steady_clock::now();

        auto connect_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0)
                .count();
        auto handshake_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1)
                .count();
        PC_LOG_INFO(
            "[RELAY] OK port={} -> {}:{} (proxy connect={}ms, socks5={}ms)",
            src_port, dest_ip_str, dest_port, connect_ms, handshake_ms);

        auto *ioc = static_cast<asio::io_context *>(
            &client_sock.get_executor().context());
        session = new RelaySession{std::move(client_sock),
                                   std::move(proxy_sock),
                                   &tracker,
                                   &strand,
                                   ioc,
                                   src_port,
                                   flow_ts,
                                   dest_ip_str,
                                   dest_port};

        auto forward = relay_direction(session, true);
        auto reverse = relay_direction(session, false);
        asio::co_spawn(ex, std::move(forward), asio::detached);
        handed_off = true;
        asio::co_spawn(ex, std::move(reverse), asio::detached);
        co_return;
    } catch (const std::exception &e) {
        PC_LOG_WARN("[RELAY] FAIL port={} -> {}:{} error: {}", src_port,
                    dest_ip_str, dest_port, e.what());
        if (session) {
            if (handed_off) {
                // The second direction did not start; account for its slot.
                finish_relay_session(session);
            } else {
                delete session;
            }
        }
    }

    if (!handed_off) {
        release_slot();
        PC_LOG_DEBUG("[RELAY] Closed port={} ({}:{})", src_port, dest_ip_str,
                     dest_port);
    }
}

} // namespace clew
