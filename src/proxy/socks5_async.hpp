#pragma once

// Async SOCKS5 client handshake using C++20 coroutines.
// Replaces blocking socks5_client.hpp.
// Protocol: RFC 1928 (SOCKS5), IPv4 CONNECT only, no auth.

#define ASIO_STANDALONE
#include <asio.hpp>
#include <asio/use_awaitable.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include "core/log.hpp"

namespace clew {

using asio::ip::tcp;

// SOCKS5 handshake as a coroutine.
// Establishes a SOCKS5 tunnel through `sock` to `dest_ip:dest_port`.
// dest_ip is in host byte order (WinDivert SOCKET layer native for IPv4).
inline asio::awaitable<void>
socks5_handshake(tcp::socket& sock, uint32_t dest_ip_host, uint16_t dest_port)
{
    // Phase 1: Greeting — no auth
    std::array<uint8_t, 3> greeting = {0x05, 0x01, 0x00};  // VER=5, 1 method, NO_AUTH
    co_await asio::async_write(sock, asio::buffer(greeting), asio::use_awaitable);

    std::array<uint8_t, 2> greeting_reply{};
    co_await asio::async_read(sock, asio::buffer(greeting_reply), asio::use_awaitable);

    if (greeting_reply[0] != 0x05 || greeting_reply[1] != 0x00) {
        throw std::runtime_error("SOCKS5 greeting failed: server rejected NO_AUTH");
    }

    // Phase 2: CONNECT request — IPv4
    std::array<uint8_t, 10> connect_req{};
    connect_req[0] = 0x05;  // VER
    connect_req[1] = 0x01;  // CMD = CONNECT
    connect_req[2] = 0x00;  // RSV
    connect_req[3] = 0x01;  // ATYP = IPv4

    // dest_ip in network byte order
    uint32_t ip_net = htonl(dest_ip_host);
    std::memcpy(&connect_req[4], &ip_net, 4);

    // dest_port in network byte order
    uint16_t port_net = htons(dest_port);
    std::memcpy(&connect_req[8], &port_net, 2);

    co_await asio::async_write(sock, asio::buffer(connect_req), asio::use_awaitable);

    // Read CONNECT reply (minimum 10 bytes for IPv4)
    std::array<uint8_t, 10> connect_reply{};
    co_await asio::async_read(sock, asio::buffer(connect_reply), asio::use_awaitable);

    if (connect_reply[0] != 0x05) {
        throw std::runtime_error("SOCKS5 connect: invalid version in reply");
    }
    if (connect_reply[1] != 0x00) {
        PC_LOG_WARN("[SOCKS5] Connect failed: reply code 0x{:02X}", connect_reply[1]);
        throw std::runtime_error("SOCKS5 connect: server returned error " +
                                 std::to_string(connect_reply[1]));
    }

    // Success — tunnel established, sock is now a transparent pipe
}

} // namespace clew
