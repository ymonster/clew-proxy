#pragma once

// PolicyTable: destination-IP exclude policy, published once globally instead
// of being snapshotted into every tracker slot.
//
// Why: a policy's content depends only on (global config, matched rule). It has
// nothing to do with the local port, so keeping a per-port snapshot copies the
// same data 65536 times and forces heap-owned state into a lock-free slot array
// (see the synchronization note in udp_port_tracker.hpp).
//
// Ownership model:
//   Writer:  strand only. On a config/rule change it builds a fresh immutable
//            table and publish()es it, bumping a generation counter.
//   Reader:  each NETWORK worker keeps a PolicyReader on its own stack. The hot
//            path is a single atomic load of the generation counter; the
//            shared_ptr is only touched when the generation actually changed.
//   Reclaim: an old table dies once the last reader has swapped to the new one.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "rules/traffic_filter.hpp"

namespace clew {

// Row 0 is reserved for "global excludes only". Manual hijacks resolve here,
// and so does any policy id that no longer maps to a live rule.
inline constexpr uint32_t GLOBAL_ONLY_POLICY = 0;

struct PolicyTable {
    std::vector<IpExcludePolicy> rows;  // rows[0] always exists

    [[nodiscard]] IpExcludeReason evaluate(uint32_t policy_id,
                                           uint32_t host_order_ip) const noexcept {
        if (rows.empty()) return IpExcludeReason::none;
        const auto& row = (policy_id < rows.size()) ? rows[policy_id]
                                                    : rows[GLOBAL_ONLY_POLICY];
        return row.evaluate(host_order_ip);
    }
};

// Publication point. One instance for the whole process.
class PolicyPublisher {
public:
    PolicyPublisher() {
        auto initial = std::make_shared<PolicyTable>();
        initial->rows.emplace_back();  // row 0, no excludes until the first publish
        table_ = std::move(initial);
    }

    // Called on the strand only.
    void publish(std::shared_ptr<const PolicyTable> table) {
        if (!table || table->rows.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        table_ = std::move(table);
        generation_.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    // Table plus the generation it belongs to, read together so a reader can
    // never record a generation that does not match the table it kept.
    [[nodiscard]] std::pair<std::shared_ptr<const PolicyTable>, uint64_t>
    snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        return {table_, generation_.load(std::memory_order_relaxed)};
    }

private:
    mutable std::mutex mu_;
    std::shared_ptr<const PolicyTable> table_;
    std::atomic<uint64_t> generation_{0};
};

// One per worker thread, kept on that worker's stack.
class PolicyReader {
public:
    explicit PolicyReader(const PolicyPublisher& publisher) : publisher_(publisher) {
        refresh();
    }

    [[nodiscard]] IpExcludeReason evaluate(uint32_t policy_id, uint32_t host_order_ip) {
        if (publisher_.generation() != seen_generation_) refresh();
        return table_->evaluate(policy_id, host_order_ip);
    }

private:
    void refresh() {
        auto [table, generation] = publisher_.snapshot();
        table_ = std::move(table);
        seen_generation_ = generation;
    }

    const PolicyPublisher& publisher_;
    std::shared_ptr<const PolicyTable> table_;
    uint64_t seen_generation_{0};
};

}  // namespace clew
