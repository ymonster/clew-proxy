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
#include <type_traits>

#include "rules/policy_table.hpp"

namespace clew {

struct UdpTrackerEntry {
    uint32_t group_id{0};
    uint32_t pid{0};                              // needed for SOCKS5 session routing
    uint32_t policy_id{GLOBAL_ONLY_POLICY};       // row in the published PolicyTable
};

// Slots are published lock-free: the writer fills the entry and then flips
// `active` with release semantics, and there is nothing stopping the writer
// from overwriting a slot a reader is still copying. That is only survivable
// while a torn read costs at most one misrouted packet, which requires the
// entry to be plain bytes. A member with a non-trivial copy (shared_ptr,
// string, vector) turns the same race into heap corruption. Heap-owned policy
// data belongs in the generation-versioned PolicyTable; keep an id here.
static_assert(std::is_trivially_copyable_v<UdpTrackerEntry>,
              "UdpTrackerEntry must stay trivially copyable — see the comment above");

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
        auto& slot = slots_[port];
        slot.active.store(false, std::memory_order_release);
        slot.entry = UdpTrackerEntry{};
    }

private:
    std::array<UdpTrackerSlot, 65536> slots_{};
};

} // namespace clew
