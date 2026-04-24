#pragma once

// SOCKS5 UDP ASSOCIATE session: owns TCP control + UDP data sockets.
// Lifecycle: establish() -> async_send_udp()/async_recv_udp() -> close()
// TCP control connection death = session invalidation (RFC 1928).
//
// All IO is async (Asio coroutines). No blocking recv on the UDP socket.

#define ASIO_STANDALONE
#include <asio.hpp>
#include <asio/use_awaitable.hpp>

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <atomic>
#include "core/log.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

namespace clew {

using asio::ip::tcp;
using asio::ip::udp;

class Socks5UdpSession : public std::enable_shared_from_this<Socks5UdpSession> {
public:
    Socks5UdpSession(asio::io_context& ioc,
                     const std::string& proxy_host, uint16_t proxy_port)
        : ioc_(ioc)
        , proxy_host_(proxy_host)
        , proxy_port_(proxy_port)
        , tcp_control_(ioc)
        , udp_data_(ioc)
    {}

    ~Socks5UdpSession() { close(); }

    // Establish SOCKS5 UDP ASSOCIATE session (blocking, call from dedicated thread).
    // Returns true on success, after which async_send_udp/async_recv_udp are usable.
    bool establish() {
        try {
            // TCP connect to SOCKS5 proxy
            tcp::resolver resolver(ioc_);
            auto endpoints = resolver.resolve(proxy_host_, std::to_string(proxy_port_));
            asio::connect(tcp_control_, endpoints);

            // Auth handshake (NO_AUTH)
            uint8_t auth_req[] = {0x05, 0x01, 0x00};
            asio::write(tcp_control_, asio::buffer(auth_req));

            uint8_t auth_resp[2]{};
            asio::read(tcp_control_, asio::buffer(auth_resp));
            if (auth_resp[0] != 0x05 || auth_resp[1] != 0x00) {
                PC_LOG_ERROR("[SOCKS5-UDP] Auth rejected");
                return false;
            }

            // Bind UDP socket to 127.0.0.1 (must declare real addr to proxy)
            udp_data_.open(udp::v4());
            udp_data_.bind(udp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
            auto local_ep = udp_data_.local_endpoint();

            // UDP ASSOCIATE request with wildcard address (0.0.0.0:0)
            // Same as standard clients (Python, curl, etc.)
            uint8_t assoc_req[] = {0x05, 0x03, 0x00, 0x01,
                                   0x00, 0x00, 0x00, 0x00,  // 0.0.0.0
                                   0x00, 0x00};             // port 0
            asio::write(tcp_control_, asio::buffer(assoc_req));

            // Parse ASSOCIATE response
            uint8_t resp[4]{};
            asio::read(tcp_control_, asio::buffer(resp));
            if (resp[1] != 0x00) {
                PC_LOG_ERROR("[SOCKS5-UDP] ASSOCIATE rejected: REP=0x{:02X}", resp[1]);
                return false;
            }

            // Parse BND.ADDR:BND.PORT
            if (resp[3] == 0x01) { // IPv4
                uint8_t bnd[6];
                asio::read(tcp_control_, asio::buffer(bnd));
                uint32_t bnd_ip;
                std::memcpy(&bnd_ip, bnd, 4);
                uint16_t bnd_port = (static_cast<uint16_t>(bnd[4]) << 8) | bnd[5];
                asio::ip::address_v4 bnd_addr(ntohl(bnd_ip));
                if (bnd_addr == asio::ip::address_v4::any())
                    bnd_addr = asio::ip::make_address_v4(proxy_host_.c_str());
                relay_endpoint_ = udp::endpoint(bnd_addr, bnd_port);
            } else if (resp[3] == 0x04) { // IPv6
                uint8_t bnd[18];
                asio::read(tcp_control_, asio::buffer(bnd));
                uint16_t bnd_port = (static_cast<uint16_t>(bnd[16]) << 8) | bnd[17];
                relay_endpoint_ = udp::endpoint(
                    asio::ip::make_address_v4(proxy_host_.c_str()), bnd_port);
            } else {
                PC_LOG_ERROR("[SOCKS5-UDP] Unsupported BND ATYP=0x{:02X}", resp[3]);
                return false;
            }

            alive_.store(true, std::memory_order_release);

            PC_LOG_INFO("[SOCKS5-UDP] Session established: local={}:{} relay={}:{}",
                         local_ep.address().to_string(), local_ep.port(),
                         relay_endpoint_.address().to_string(), relay_endpoint_.port());

            // TCP watchdog: async read on the control socket; when it returns
            // (EOF / reset / any error), flip alive_ false. Managed by the
            // io_context that owns tcp_control_, no dedicated thread needed.
            asio::co_spawn(tcp_control_.get_executor(),
                [self = shared_from_this()]() -> asio::awaitable<void> {
                    uint8_t dummy[1];
                    auto [ec, n] = co_await self->tcp_control_.async_read_some(
                        asio::buffer(dummy), asio::as_tuple(asio::use_awaitable));
                    if (ec)
                        PC_LOG_DEBUG("[SOCKS5-UDP] TCP watchdog detected control loss: {}",
                                     ec.message());
                    if (self->alive_.exchange(false))
                        PC_LOG_WARN("[SOCKS5-UDP] TCP control connection lost");
                }, asio::detached);

            return true;

        } catch (const std::exception& e) {
            PC_LOG_ERROR("[SOCKS5-UDP] Establish failed: {}", e.what());
            return false;
        }
    }

    // ---- Async IO (the only IO interfaces) ----

    asio::awaitable<bool> async_send_udp(std::vector<uint8_t> frame) {
        if (!alive_.load(std::memory_order_acquire)) co_return false;
        auto local_port = udp_data_.local_endpoint().port();
        auto [ec, n] = co_await udp_data_.async_send_to(
            asio::buffer(frame), relay_endpoint_,
            asio::as_tuple(asio::use_awaitable));
        if (!ec) {
            last_active_ = std::chrono::steady_clock::now();
            PC_LOG_INFO("[SOCKS5-UDP] async_send {} bytes from local:{} -> {}:{}",
                         n, local_port,
                         relay_endpoint_.address().to_string(), relay_endpoint_.port());
        }
        co_return !ec;
    }

    asio::awaitable<std::pair<size_t, asio::error_code>>
    async_recv_udp(uint8_t* buf, size_t buf_len) {
        udp::endpoint from;
        auto [ec, n] = co_await udp_data_.async_receive_from(
            asio::buffer(buf, buf_len), from,
            asio::as_tuple(asio::use_awaitable));
        if (!ec) last_active_ = std::chrono::steady_clock::now();
        co_return std::pair{n, ec};
    }

    // ---- Warmup send (fire-and-forget, called from non-io thread via raw sendto) ----

    bool send_warmup(const std::vector<uint8_t>& frame) {
        if (!alive_.load(std::memory_order_acquire)) return false;
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(relay_endpoint_.port());
        dest.sin_addr.s_addr = htonl(relay_endpoint_.address().to_v4().to_uint());
        int sent = ::sendto(udp_data_.native_handle(),
                            reinterpret_cast<const char*>(frame.data()),
                            static_cast<int>(frame.size()), 0,
                            reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
        if (sent > 0) last_active_ = std::chrono::steady_clock::now();
        return sent > 0;
    }

    // ---- Accessors ----

    bool is_alive() const { return alive_.load(std::memory_order_acquire); }
    auto last_active() const { return last_active_.load(); }
    const udp::endpoint& relay_endpoint() const { return relay_endpoint_; }
    auto native_udp_handle() { return udp_data_.native_handle(); }

    void close() {
        alive_.store(false, std::memory_order_release);
        asio::error_code ec;
        tcp_control_.close(ec);
        udp_data_.close(ec);
    }

private:
    asio::io_context& ioc_;
    std::string proxy_host_;
    uint16_t proxy_port_;

    tcp::socket tcp_control_;
    udp::socket udp_data_;
    udp::endpoint relay_endpoint_;

    std::atomic<bool> alive_{false};
    std::atomic<std::chrono::steady_clock::time_point> last_active_{
        std::chrono::steady_clock::now()};

};

} // namespace clew
