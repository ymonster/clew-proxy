#pragma once

// DNS Forwarder: listens on a local UDP port for DNS queries,
// forwards them through a SOCKS5 UDP ASSOCIATE session to a real
// DNS server (e.g., 8.8.8.8:53), and returns responses.
//
// This ensures DNS resolution happens at the proxy exit, providing
// geographic consistency and bypassing local DNS pollution/blocking.

#define ASIO_STANDALONE
#include <asio.hpp>
#include <asio/use_awaitable.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <memory>
#include "core/log.hpp"
#include "udp/socks5_udp_session.hpp"
#include "udp/socks5_udp_codec.hpp"

namespace clew {

class dns_forwarder {
public:
    dns_forwarder(asio::io_context& ioc,
                  const std::string& listen_ip, uint16_t listen_port,
                  const std::string& proxy_host, uint16_t proxy_port,
                  const std::string& dns_server = "8.8.8.8", uint16_t dns_port = 53)
        : ioc_(ioc)
        , listen_ep_(asio::ip::make_address(listen_ip), listen_port)
        , proxy_host_(proxy_host), proxy_port_(proxy_port)
        , dns_server_(dns_server), dns_port_(dns_port)
        , listen_sock_(ioc)
    {}

    bool start() {
        try {
            listen_sock_.open(asio::ip::udp::v4());
            listen_sock_.set_option(asio::socket_base::reuse_address(true));
            listen_sock_.bind(listen_ep_);

            running_ = true;

            // Establish SOCKS5 UDP ASSOCIATE session for DNS forwarding
            socks5_session_ = std::make_shared<Socks5UdpSession>(ioc_, proxy_host_, proxy_port_);
            if (!socks5_session_->establish()) {
                PC_LOG_ERROR("[DNS-FWD] Failed to establish SOCKS5 session to {}:{}",
                              proxy_host_, proxy_port_);
                return false;
            }

            // Parse DNS server IP for SOCKS5 encoding
            auto addr = asio::ip::make_address_v4(dns_server_);
            dns_ip_net_ = htonl(addr.to_uint());

            asio::co_spawn(ioc_, query_loop(), asio::detached);
            asio::co_spawn(ioc_, response_loop(), asio::detached);

            PC_LOG_INFO("[DNS-FWD] Listening on {}:{} -> SOCKS5 {}:{} -> {}:{}",
                         listen_ep_.address().to_string(), listen_ep_.port(),
                         proxy_host_, proxy_port_, dns_server_, dns_port_);
            return true;
        } catch (const std::exception& e) {
            PC_LOG_ERROR("[DNS-FWD] Start failed: {}", e.what());
            return false;
        }
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        asio::error_code ec;
        listen_sock_.close(ec);
        if (socks5_session_) socks5_session_->close();
        PC_LOG_INFO("[DNS-FWD] Stopped (queries={}, responses={})",
                     query_count_.load(), response_count_.load());
    }

private:
    asio::io_context& ioc_;
    asio::ip::udp::endpoint listen_ep_;
    std::string proxy_host_;
    uint16_t proxy_port_;
    std::string dns_server_;
    uint16_t dns_port_;
    uint32_t dns_ip_net_{0};  // network byte order

    asio::ip::udp::socket listen_sock_;
    std::shared_ptr<Socks5UdpSession> socks5_session_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> query_count_{0};
    std::atomic<uint64_t> response_count_{0};

    // Map DNS tx_id → original client endpoint for routing responses back
    struct PendingQuery {
        asio::ip::udp::endpoint client_ep;
        std::chrono::steady_clock::time_point sent_at;
    };
    std::unordered_map<uint16_t, PendingQuery> pending_;

    // Extract the queried domain name from a DNS query message for logging.
    // Returns empty string if the message is too short or malformed.
    static std::string parse_dns_qname(const uint8_t* buf, size_t n) {
        std::string qname;
        if (n <= 12) return qname;
        size_t pos = 12;
        while (pos < n && buf[pos] != 0) {
            uint8_t len = buf[pos++];
            if (pos + len > n) break;
            if (!qname.empty()) qname += '.';
            qname.append(reinterpret_cast<const char*>(buf + pos), len);
            pos += len;
        }
        return qname;
    }

    // Client → SOCKS5 → DNS server
    asio::awaitable<void> query_loop() {
        uint8_t buf[4096];
        asio::ip::udp::endpoint client_ep;

        while (running_) {
            auto [ec, n] = co_await listen_sock_.async_receive_from(
                asio::buffer(buf), client_ep,
                asio::as_tuple(asio::use_awaitable));
            if (ec) {
                // WSAECONNRESET (10054): Windows returns this on UDP when an ICMP
                // Port Unreachable is received. This is normal and must not break
                // the receive loop.
                if (ec == asio::error::connection_reset ||
                    ec == asio::error::connection_refused) {
                    continue;
                }
                if (running_) PC_LOG_WARN("[DNS-FWD] listen recv error: {}", ec.message());
                break;
            }
            if (n < 12) continue;  // too short for DNS header

            uint16_t tx_id = (static_cast<uint16_t>(buf[0]) << 8) | buf[1];

            std::string qname = parse_dns_qname(buf, n);
            PC_LOG_DEBUG("[DNS-FWD] Q tx={:#06x} {} from {}:{}",
                          tx_id, qname, client_ep.address().to_string(), client_ep.port());

            pending_[tx_id] = {client_ep, std::chrono::steady_clock::now()};

            // Reconnect SOCKS5 session if dead
            if (!socks5_session_ || !socks5_session_->is_alive()) {
                socks5_session_ = std::make_shared<Socks5UdpSession>(ioc_, proxy_host_, proxy_port_);
                if (!socks5_session_->establish()) {
                    PC_LOG_ERROR("[DNS-FWD] SOCKS5 reconnect failed");
                    continue;
                }
                // Re-spawn response loop for new session
                asio::co_spawn(ioc_, response_loop(), asio::detached);
            }

            // Encode as SOCKS5 UDP frame and send
            auto frame = socks5_udp::encode(dns_ip_net_, dns_port_, buf, n);
            bool sent = co_await socks5_session_->async_send_udp(std::move(frame));

            if (sent) {
                query_count_.fetch_add(1, std::memory_order_relaxed);
            } else {
                PC_LOG_WARN("[DNS-FWD] Failed to send DNS query tx_id={:#06x}", tx_id);
            }

            // Periodic cleanup
            if (query_count_ % 32 == 0) {
                auto now = std::chrono::steady_clock::now();
                std::erase_if(pending_, [now](const auto& kv) {
                    return now - kv.second.sent_at > std::chrono::seconds(10);
                });
            }
        }
    }

    // SOCKS5 → Client (DNS responses)
    asio::awaitable<void> response_loop() {
        uint8_t buf[4096];

        while (running_ && socks5_session_ && socks5_session_->is_alive()) {
            auto [n, ec] = co_await socks5_session_->async_recv_udp(buf, sizeof(buf));
            if (ec) {
                if (ec == asio::error::connection_reset ||
                    ec == asio::error::connection_refused) {
                    continue;
                }
                if (running_ && socks5_session_->is_alive())
                    PC_LOG_WARN("[DNS-FWD] SOCKS5 recv error: {}", ec.message());
                break;
            }
            if (n == 0) continue;

            // Decode SOCKS5 UDP frame
            auto frame = socks5_udp::decode(buf, n);
            if (!frame || frame->data_len < 12) continue;

            uint16_t tx_id = (static_cast<uint16_t>(frame->data[0]) << 8) | frame->data[1];

            auto it = pending_.find(tx_id);
            if (it == pending_.end()) {
                PC_LOG_DEBUG("[DNS-FWD] No pending query for tx_id={:#06x}", tx_id);
                continue;
            }

            auto client_ep = it->second.client_ep;
            auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - it->second.sent_at).count();
            pending_.erase(it);

            uint16_t rcode = (frame->data_len >= 4)
                ? (static_cast<uint16_t>(frame->data[2] & 0x0F) << 8 | frame->data[3]) & 0xF
                : 0;
            uint16_t ancount = (frame->data_len >= 8)
                ? (static_cast<uint16_t>(frame->data[6]) << 8 | frame->data[7])
                : 0;
            PC_LOG_DEBUG("[DNS-FWD] R tx={:#06x} rcode={} answers={} {}ms",
                          tx_id, rcode, ancount, latency_ms);

            // Send DNS response back to original client
            co_await listen_sock_.async_send_to(
                asio::buffer(frame->data, frame->data_len), client_ep,
                asio::use_awaitable);

            response_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

} // namespace clew
