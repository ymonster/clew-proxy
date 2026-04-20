#pragma once

// UdpPortTracker: 65536 fixed-size array for O(1) lock-free UDP port->session mapping.
// Independent from TCP PortTracker — separate memory, separate lifetime.
//
// Synchronization model (mirrors TCP PortTracker):
//   Writer: UDP SOCKET handler (strand, single thread)
//   Reader: UDP NETWORK worker threads (2, read-only check)
//   Guarantee: release/acquire on atomic<bool> ensures complete entry visibility

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <optional>
#include <cstdint>
#include <cstring>

namespace clew {

struct UdpTrackerEntry {
    uint32_t remote_addr[4]{};  // host byte order (WinDivert native); IPv4 in [0]
    uint16_t remote_port{0};    // host byte order
    uint32_t group_id{0};
    uint32_t pid{0};            // needed for SOCKS5 session routing
};

struct alignas(64) UdpTrackerSlot {
    std::atomic<bool> active{false};
    UdpTrackerEntry entry{};
};

class UdpPortTracker {
public:
    // Write entry (called from UDP SOCKET handler on strand)
    void put(uint16_t src_port, const UdpTrackerEntry& e) {
        auto& slot = slots_[src_port];
        slot.entry = e;
        slot.active.store(true, std::memory_order_release);
    }

    // Check if port has active entry (called from UDP NETWORK workers)
    bool is_active(uint16_t port) const {
        return slots_[port].active.load(std::memory_order_acquire);
    }

    // Read entry without consuming (called from UDP NETWORK workers)
    const UdpTrackerEntry& peek(uint16_t port) const {
        return slots_[port].entry;
    }

    // Read entry if active
    std::optional<UdpTrackerEntry> get(uint16_t port) const {
        auto& slot = slots_[port];
        if (slot.active.load(std::memory_order_acquire))
            return slot.entry;
        return std::nullopt;
    }

    // Clear entry (called when UDP socket closes or session expires)
    void clear(uint16_t port) {
        slots_[port].active.store(false, std::memory_order_release);
    }

private:
    std::array<UdpTrackerSlot, 65536> slots_{};
};

} // namespace clew
