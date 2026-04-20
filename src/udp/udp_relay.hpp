#pragma once

// UDP Relay: receives reflected UDP packets, forwards via SOCKS5 UDP ASSOCIATE,
// and injects replies back to the application via WinDivert.
//
// Architecture (per-local-port sessions):
//   upstream_loop (coroutine): relay_socket recv -> lookup session table -> get/create SOCKS5 session -> proxy
//   downstream_for_session (coroutine, per-app_port): proxy -> SOCKS5 decode -> inject back to app
//
// Each hijacked UDP socket gets its own SOCKS5 UDP ASSOCIATE session (RFC 1928 compliant).
// Responses route directly to the owning app_port — no tx_id/addr correlation needed.

#define ASIO_STANDALONE
#include <asio.hpp>
#include <asio/use_awaitable.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windivert.h>

#include <cstdint>
#include <cstring>
#include <vector>
#include <atomic>
#include "core/log.hpp"

#include "udp/udp_session_table.hpp"
#include "udp/socks5_udp_manager.hpp"
#include "udp/socks5_udp_codec.hpp"

namespace clew {

class UdpRelay {
public:
    UdpRelay(asio::io_context& ioc,
             UdpSessionTable& session_table,
             Socks5UdpManager& socks5_mgr,
             HANDLE wd_handle,
             uint16_t relay_port)
        : ioc_(ioc)
        , session_table_(session_table)
        , socks5_mgr_(socks5_mgr)
        , wd_handle_(wd_handle)
        , relay_port_(relay_port)
        , relay_socket_(ioc)
    {
        // When a new per-port SOCKS5 session is created, spawn its downstream coroutine
        socks5_mgr_.set_on_session_created(
            [this](uint16_t app_port, std::shared_ptr<Socks5UdpSession> session) {
                spawn_downstream(app_port, std::move(session));
            });
    }

    ~UdpRelay() { stop(); }

    bool start() {
        try {
            relay_socket_.open(asio::ip::udp::v4());
            relay_socket_.bind(asio::ip::udp::endpoint(
                asio::ip::address_v4::any(), relay_port_));

            PC_LOG_INFO("[UDP-RELAY] Listening on port {}", relay_port_);
            running_ = true;

            asio::co_spawn(ioc_, upstream_loop(), asio::detached);

            return true;
        } catch (const std::exception& e) {
            PC_LOG_ERROR("[UDP-RELAY] Start failed: {}", e.what());
            return false;
        }
    }

    // Shutdown order requirement:
    //   1. socks5_mgr.stop()  — closes sessions, causes downstream coroutines to error out
    //   2. udp_relay->stop()  — clears callback, closes socket, breaks upstream_loop
    //   3. ioc.stop() + workers.join() — ensures all coroutines have exited
    //   4. UdpRelay destructor runs safely (all coroutines already completed)
    void stop() {
        if (stopped_) return;
        stopped_ = true;
        running_ = false;

        socks5_mgr_.set_on_session_created(nullptr);

        asio::error_code ec;
        relay_socket_.close(ec);

        PC_LOG_INFO("[UDP-RELAY] Stopped (up={}, down={}, injected={})",
                     upstream_count_.load(), downstream_count_.load(), injected_count_.load());
    }

    uint64_t upstream_count() const { return upstream_count_; }
    uint64_t downstream_count() const { return downstream_count_; }
    uint64_t injected_count() const { return injected_count_; }

private:
    asio::io_context& ioc_;
    UdpSessionTable& session_table_;
    Socks5UdpManager& socks5_mgr_;
    HANDLE wd_handle_;
    uint16_t relay_port_;

    asio::ip::udp::socket relay_socket_;
    bool stopped_{false};
    std::atomic<bool> running_{false};

    std::atomic<uint64_t> upstream_count_{0};
    std::atomic<uint64_t> downstream_count_{0};
    std::atomic<uint64_t> injected_count_{0};

    // Spawn a downstream coroutine for a newly created per-port session
    void spawn_downstream(uint16_t app_port, std::shared_ptr<Socks5UdpSession> session) {
        asio::co_spawn(ioc_,
            downstream_for_session(app_port, std::move(session)),
            asio::detached);
    }

    // ---- Upstream: relay_socket -> SOCKS5 -> proxy ----

    asio::awaitable<void> upstream_loop() {
        uint8_t buf[65536];
        asio::ip::udp::endpoint sender;

        while (running_) {
            auto [ec, n] = co_await relay_socket_.async_receive_from(
                asio::buffer(buf), sender,
                asio::as_tuple(asio::use_awaitable));
            if (ec) {
                if (running_)
                    PC_LOG_DEBUG("[UDP-RELAY] upstream recv error: {}", ec.message());
                break;
            }

            uint16_t app_port = sender.port();

            auto session = session_table_.lookup(app_port);
            if (!session) continue;

            // Get or create per-port SOCKS5 session
            auto socks5 = socks5_mgr_.get_or_create(app_port, session->group_id);
            if (!socks5) {
                PC_LOG_WARN("[UDP-RELAY] No SOCKS5 session for port={} group={}",
                             app_port, session->group_id);
                continue;
            }

            uint16_t dst_port_host = ntohs(session->orig_dst_port);

            auto frame = socks5_udp::encode(
                session->orig_dst_addr, dst_port_host,
                buf, static_cast<size_t>(n));

            bool sent = co_await socks5->async_send_udp(frame);
            PC_LOG_INFO("[UDP-RELAY] UP app_port={} dst_port={} {} bytes sent={}",
                         app_port, dst_port_host, frame.size(), sent);
            if (sent) {
                upstream_count_.fetch_add(1, std::memory_order_relaxed);
                session_table_.touch(app_port);
            }
        }
    }

    // ---- Downstream: per-port async coroutine ----
    // Each app_port has its own SOCKS5 session, so responses are 1:1 — no matching needed.

    asio::awaitable<void> downstream_for_session(
        uint16_t app_port, std::shared_ptr<Socks5UdpSession> session)
    {
        PC_LOG_INFO("[UDP-RELAY] Downstream started for port={}", app_port);
        uint8_t buf[65536];

        while (session->is_alive() && running_) {
            auto [n, ec] = co_await session->async_recv_udp(buf, sizeof(buf));
            if (ec) {
                if (session->is_alive() && running_)
                    PC_LOG_DEBUG("[UDP-RELAY] downstream recv error port={}: {}",
                                  app_port, ec.message());
                if (!session->is_alive()) break;
                continue;
            }
            if (n == 0) continue;

            downstream_count_.fetch_add(1, std::memory_order_relaxed);

            auto frame = socks5_udp::decode(buf, n);
            if (!frame) {
                PC_LOG_DEBUG("[UDP-RELAY] Bad SOCKS5 frame ({} bytes) port={}", n, app_port);
                continue;
            }

            // Direct routing: this session belongs to app_port, inject reply there
            auto s = session_table_.lookup(app_port);
            if (!s) {
                PC_LOG_DEBUG("[UDP-RELAY] No session table entry for port={}", app_port);
                continue;
            }

            PC_LOG_INFO("[UDP-RELAY] DOWN port={} {} bytes", app_port, frame->data_len);
            inject_reply(*s, frame->data, frame->data_len);
        }

        PC_LOG_INFO("[UDP-RELAY] Downstream ended for port={}", app_port);
    }

    // Construct and inject a spoofed inbound UDP packet
    void inject_reply(const UdpSession& sess,
                      const uint8_t* payload, size_t payload_len)
    {
        if (wd_handle_ == INVALID_HANDLE_VALUE) return;

        size_t pkt_size = 20 + 8 + payload_len;
        std::vector<uint8_t> pkt(pkt_size, 0);

        auto* ip = reinterpret_cast<WINDIVERT_IPHDR*>(pkt.data());
        ip->Version   = 4;
        ip->HdrLength = 5;
        ip->Length     = htons(static_cast<uint16_t>(pkt_size));
        ip->Id         = htons(static_cast<uint16_t>(GetTickCount() & 0xFFFF));
        ip->TTL        = 64;
        ip->Protocol   = 17; // UDP
        ip->SrcAddr    = sess.orig_dst_addr;
        ip->DstAddr    = sess.app_src_addr;

        auto* uh = reinterpret_cast<WINDIVERT_UDPHDR*>(pkt.data() + 20);
        uh->SrcPort = sess.orig_dst_port;
        uh->DstPort = htons(sess.app_src_port);
        uh->Length  = htons(static_cast<uint16_t>(8 + payload_len));

        std::memcpy(pkt.data() + 28, payload, payload_len);

        WINDIVERT_ADDRESS wd_addr{};
        wd_addr.Outbound = FALSE;
        wd_addr.Network.IfIdx = sess.ifidx;
        wd_addr.Network.SubIfIdx = sess.subifidx;

        WinDivertHelperCalcChecksums(pkt.data(), static_cast<UINT>(pkt_size), &wd_addr, 0);

        UINT sent = 0;
        if (WinDivertSend(wd_handle_, pkt.data(), static_cast<UINT>(pkt_size), &sent, &wd_addr)) {
            injected_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            PC_LOG_WARN("[UDP-RELAY] Inject failed: {}", GetLastError());
        }
    }
};

} // namespace clew
