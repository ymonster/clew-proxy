#pragma once

// PortTracker: 65536 fixed-size array for O(1) lock-free port→connection mapping.
// Replaces mutex+unordered_map ConnectionTracker from v1.
//
// Synchronization model (from architecture_redesign_v3.md §4):
//   Writer: SOCKET SNIFF handler (strand, single thread)
//   Reader 1: NETWORK worker threads (2-4, read-only check)
//   Reader 2: Acceptor coroutine (read + clear via take())
//   Guarantee: release/acquire on atomic<bool> ensures NETWORK workers
//              see complete entry data when active==true

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

struct TrackerEntry {
    uint32_t remote_addr[4]{};  // host byte order (WinDivert SOCKET layer native); IPv4 in [0]
    uint16_t remote_port{0};    // host byte order
    uint32_t group_id{0};
};

struct alignas(64) TrackerSlot {
    std::atomic<bool> active{false};
    TrackerEntry entry{};
};

class PortTracker {
public:
    // Direct slot access (for SOCKET handler write + NETWORK worker read)
    TrackerSlot& operator[](uint16_t port) { return slots_[port]; }
    const TrackerSlot& operator[](uint16_t port) const { return slots_[port]; }

    // Write a tracker entry (called from SOCKET handler on strand)
    void put(uint16_t src_port, const TrackerEntry& e) {
        auto& slot = slots_[src_port];
        slot.entry = e;
        slot.active.store(true, std::memory_order_release);
    }

    // Check if a port has an active entry (called from NETWORK workers)
    bool is_active(uint16_t port) const {
        return slots_[port].active.load(std::memory_order_acquire);
    }

    // Read entry without consuming (called from NETWORK workers)
    const TrackerEntry& peek(uint16_t port) const {
        return slots_[port].entry;
    }

    // Read entry without clearing (called from acceptor coroutine).
    // Entry stays active for NETWORK layer to continue NAT.
    std::optional<TrackerEntry> take(uint16_t port) {
        auto& slot = slots_[port];
        if (slot.active.load(std::memory_order_acquire)) {
            return slot.entry;
        }
        return std::nullopt;
    }

    // Explicitly clear an entry (called when connection is fully closed)
    void clear(uint16_t port) {
        slots_[port].active.store(false, std::memory_order_release);
    }

private:
    std::array<TrackerSlot, 65536> slots_{};
};

} // namespace clew
