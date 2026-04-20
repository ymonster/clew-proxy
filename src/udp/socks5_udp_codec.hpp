#pragma once

// SOCKS5 UDP frame encode/decode (RFC 1928 section 7).
// Header-only, no external dependencies.
//
// Frame format:
//   +----+------+------+----------+----------+----------+
//   |RSV | FRAG | ATYP | DST.ADDR | DST.PORT |   DATA   |
//   +----+------+------+----------+----------+----------+
//   | 2  |  1   |  1   | Variable |    2     | Variable |
//   +----+------+------+----------+----------+----------+

#include <cstdint>
#include <cstring>
#include <vector>
#include <optional>

namespace clew {

struct Socks5UdpFrame {
    uint32_t addr_net{};    // network byte order
    uint16_t port_host{};   // host byte order
    const uint8_t* data{};
    size_t data_len{};
};

namespace socks5_udp {

// Encode: raw UDP payload -> SOCKS5 UDP frame.
// dst_ip_net: destination IP in network byte order (raw from IP header).
// dst_port_host: destination port in host byte order.
inline std::vector<uint8_t> encode(
    uint32_t dst_ip_net, uint16_t dst_port_host,
    const uint8_t* payload, size_t payload_len)
{
    std::vector<uint8_t> frame;
    frame.reserve(10 + payload_len);
    frame.push_back(0x00); // RSV
    frame.push_back(0x00); // RSV
    frame.push_back(0x00); // FRAG = 0 (no fragmentation)
    frame.push_back(0x01); // ATYP = IPv4

    // DST.ADDR: raw bytes (already network byte order)
    auto* ip_bytes = reinterpret_cast<const uint8_t*>(&dst_ip_net);
    frame.insert(frame.end(), ip_bytes, ip_bytes + 4);

    // DST.PORT: big-endian (network byte order)
    frame.push_back(static_cast<uint8_t>(dst_port_host >> 8));
    frame.push_back(static_cast<uint8_t>(dst_port_host & 0xFF));

    // DATA
    frame.insert(frame.end(), payload, payload + payload_len);
    return frame;
}

// Decode: SOCKS5 UDP frame -> raw payload + addressing info.
// Returns nullopt on invalid/unsupported frames.
inline std::optional<Socks5UdpFrame> decode(const uint8_t* buf, size_t len) {
    if (len < 10) return std::nullopt;

    // FRAG must be 0 (fragmented frames not supported)
    if (buf[2] != 0x00) return std::nullopt;

    uint8_t atyp = buf[3];
    Socks5UdpFrame frame{};

    if (atyp == 0x01) { // IPv4
        if (len < 10) return std::nullopt;
        std::memcpy(&frame.addr_net, &buf[4], 4);
        frame.port_host = (static_cast<uint16_t>(buf[8]) << 8) | buf[9];
        frame.data = buf + 10;
        frame.data_len = len - 10;
    } else if (atyp == 0x04) { // IPv6 — skip for now, log and return nullopt
        return std::nullopt;
    } else if (atyp == 0x03) { // Domain — skip for now
        return std::nullopt;
    } else {
        return std::nullopt;
    }

    return frame;
}

} // namespace socks5_udp
} // namespace clew
