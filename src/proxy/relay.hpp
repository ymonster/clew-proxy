#pragma once

// Bidirectional relay using C++20 coroutines.
// Replaces thread-per-direction proxy_relay.hpp.
// Uses awaitable_operators || for any-completes-cancels-other.
//
// 100 connections = 100 coroutines on io_context worker threads.
// Previous: 100 connections = 200 threads.

#define ASIO_STANDALONE
#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/use_awaitable.hpp>

#include <array>
#include <format>
#include "core/log.hpp"

#include "core/port_tracker.hpp"
#include "proxy/socks5_async.hpp"

// Format host-byte-order IPv4 as dotted string
inline std::string ip_host_to_string(uint32_t ip) {
    return std::format("{}.{}.{}.{}",
                       (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                       (ip >> 8) & 0xFF,  ip & 0xFF);
}

namespace clew {

using asio::ip::tcp;
using asio::experimental::awaitable_operators::operator||;

// ProxyGroupConfig for relay (matches v3 architecture groups_ map value)
struct ProxyGroupConfig {
    std::string host;
    uint16_t    port{7890};
};

// Unidirectional pipe: read from `from`, write to `to`.
// On EOF or error, shutdown the write side of `to` so the other pipe sees EOF.
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
        // Any error (EOF, reset, timeout, bad_alloc) → shutdown the peer
        // direction so the other pipe coroutine sees EOF and exits. The
        // handling is identical for every exception type, so a catch-all
        // is correct here by design (not defensive laziness).
        std::error_code ec;
        to.shutdown(tcp::socket::shutdown_send, ec);
    }
}

// Handle a single redirected connection.
// Reads tracker entry, connects to SOCKS5 proxy, relays bidirectionally.
//
// Parameters:
//   client_sock:    accepted socket from local listener (redirected app connection)
//   tracker:        port tracker to lookup original destination
//   strand:         strand protecting groups_ map
//   groups:         proxy group config map (read on strand)
//   port_cleaner:   callback to clear tracker entry on connection close
inline asio::awaitable<void>
handle_connection(tcp::socket client_sock,
                  PortTracker& tracker,
                  asio::strand<asio::io_context::executor_type>& strand,
                  const std::unordered_map<uint32_t, ProxyGroupConfig>& groups)
{
    auto ex = co_await asio::this_coro::executor;

    std::error_code ep_ec;
    auto ep = client_sock.remote_endpoint(ep_ec);
    if (ep_ec) co_return;  // socket already closed
    const uint16_t src_port = ep.port();

    // Lookup tracker entry
    auto entry = tracker.take(src_port);
    if (!entry) {
        PC_LOG_DEBUG("[RELAY] No tracker entry for port {}", src_port);
        co_return;
    }

    // Query group config on strand (enter strand, read, exit immediately)
    struct GroupInfo {
        std::string host;
        uint16_t port;
    };

    auto cfg_opt = co_await asio::co_spawn(strand,
        [&groups, group_id = entry->group_id]() -> asio::awaitable<std::optional<GroupInfo>> {
            auto it = groups.find(group_id);
            if (it == groups.end()) co_return std::nullopt;
            co_return GroupInfo{it->second.host, it->second.port};
        }, asio::use_awaitable);

    if (!cfg_opt) {
        PC_LOG_WARN("[RELAY] Group {} not found for port {}", entry->group_id, src_port);
        tracker.clear(src_port);
        co_return;
    }

    auto dest_addr = entry->remote_addr[0];
    auto dest_port = entry->remote_port;

    // DNS redirect: TCP:53 connections from proxied processes → use clean DNS server
    // Prevents DNS pollution when Chromium's built-in resolver queries local DNS directly
    if (dest_port == 53) {
        // 8.8.8.8 in host byte order = 0x08080808
        auto orig = dest_addr;
        dest_addr = 0x08080808;
        PC_LOG_DEBUG("[RELAY] DNS redirect port={} {}:53 -> 8.8.8.8:53",
                      src_port, ip_host_to_string(orig));
    }

    auto dest_ip_str = ip_host_to_string(dest_addr);

    try {
        auto t0 = std::chrono::steady_clock::now();

        // Connect to SOCKS5 proxy
        tcp::resolver resolver{ex};
        auto eps = co_await resolver.async_resolve(
            cfg_opt->host, std::to_string(cfg_opt->port), asio::use_awaitable);

        tcp::socket proxy_sock{ex};
        co_await asio::async_connect(proxy_sock, eps, asio::use_awaitable);

        auto t1 = std::chrono::steady_clock::now();

        // SOCKS5 handshake — establish tunnel to original destination
        co_await socks5_handshake(proxy_sock, dest_addr, dest_port);

        auto t2 = std::chrono::steady_clock::now();
        auto connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        auto handshake_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

        PC_LOG_INFO("[RELAY] OK port={} -> {}:{} (proxy connect={}ms, socks5={}ms)",
                     src_port, dest_ip_str, dest_port, connect_ms, handshake_ms);

        // Bidirectional relay: || means any-completes-cancels-other
        // (parens required — co_await has higher precedence than ||, without them
        //  the expression parses as `(co_await pipe(a,b)) || pipe(b,a)`)
        co_await (pipe(client_sock, proxy_sock) || pipe(proxy_sock, client_sock));

    } catch (const std::exception& e) {
        PC_LOG_WARN("[RELAY] FAIL port={} -> {}:{} error: {}",
                     src_port, dest_ip_str, dest_port, e.what());
    }

    // Cleanup tracker entry
    tracker.clear(src_port);
    PC_LOG_DEBUG("[RELAY] Closed port={} ({}:{})", src_port, dest_ip_str, dest_port);
}

} // namespace clew
