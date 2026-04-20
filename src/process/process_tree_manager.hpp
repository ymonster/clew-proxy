#pragma once

// Central coordinator for process tree management.
// Owns flat_tree, ETW consumer, NtQuery reconciliation timer.
// All tree mutations happen on a single Asio strand — no locks needed.
//
// Startup sequence (from architecture_redesign_v3.md §5.1):
//   t0: ETW consumer thread starts → events buffered while !tree_initialized_
//   t1: NtQuery snapshot in thread pool
//   t2: On strand: build tree → replay ETW buffer → tree_initialized_ = true
//   t3: Normal operation: ETW real-time + 30s reconcile timer

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#define ASIO_STANDALONE
#include <asio.hpp>
#include <asio/steady_timer.hpp>

#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include "core/log.hpp"

#include "process/flat_tree.hpp"
#include "process/etw_consumer.hpp"
#include "process/ntquery_snapshot.hpp"
#include "rules/rule_engine_v3.hpp"

namespace clew {

class process_tree_manager {
public:
    using on_tree_changed_fn = std::function<void()>;

    process_tree_manager(asio::io_context& ioc)
        : ioc_(ioc)
        , strand_(asio::make_strand(ioc))
        , reconcile_timer_(ioc)
    {}

    ~process_tree_manager() { stop(); }

    // Set callback for tree changes (used to publish API snapshots, SSE events)
    void set_on_tree_changed(on_tree_changed_fn fn) { on_tree_changed_ = std::move(fn); }

    // Start the initialization sequence (t0 → t3)
    void start() {
        if (started_) return;
        started_ = true;

        PC_LOG_INFO("[TreeMgr] Starting initialization sequence...");

        // t0: Start ETW consumer — events buffered until tree is initialized
        etw_ = std::make_unique<etw_consumer>([this](const etw_process_event& evt) {
            // Thread-safe: post to strand
            asio::post(strand_, [this, evt]() {
                on_etw_event(evt);
            });
        });

        if (!etw_->start()) {
            PC_LOG_ERROR("[TreeMgr] ETW consumer failed to start");
            return;
        }

        // t1: NtQuery snapshot in thread pool (non-blocking)
        asio::post(ioc_, [this]() {
            auto snapshot = ntquery_enumerate_processes();

            // t2: Build tree on strand
            asio::post(strand_, [this, snap = std::move(snapshot)]() {
                build_initial_tree(snap);
            });
        });
    }

    void stop() {
        if (!started_) return;
        started_ = false;

        reconcile_timer_.cancel();
        if (etw_) etw_->stop();

        PC_LOG_INFO("[TreeMgr] Stopped");
    }

    // === Strand-safe accessors (call from strand or use post+future) ===

    const flat_tree& tree() const { return tree_; }
    flat_tree& tree() { return tree_; }

    rule_engine_v3& rules() { return rules_; }
    const rule_engine_v3& rules() const { return rules_; }

    asio::strand<asio::io_context::executor_type>& strand() { return strand_; }

    bool is_initialized() const { return tree_initialized_; }

    // Publish current tree as JSON (called on strand after tree changes)
    std::string snapshot_json() const {
        return tree_.to_json();
    }

private:
    asio::io_context& ioc_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer reconcile_timer_;

    flat_tree tree_;
    rule_engine_v3 rules_;
    std::unique_ptr<etw_consumer> etw_;

    bool started_{false};
    bool tree_initialized_{false};  // only accessed on strand

    // ETW event buffer (pre-initialization)
    static constexpr size_t ETW_BUFFER_LIMIT = 2048;
    std::vector<etw_process_event> etw_buffer_;

    on_tree_changed_fn on_tree_changed_;

    // ---- Startup sequence ----

    void build_initial_tree(const std::vector<raw_process_record>& snapshot) {
        // This runs on the strand

        tree_.build_from_snapshot(snapshot);
        PC_LOG_INFO("[TreeMgr] Initial tree built: {} processes", tree_.alive_count());

        // Replay buffered ETW events
        size_t replayed = 0;
        for (const auto& evt : etw_buffer_) {
            apply_etw_event(evt);
            replayed++;
        }
        if (replayed > 0)
            PC_LOG_INFO("[TreeMgr] Replayed {} buffered ETW events", replayed);

        etw_buffer_.clear();
        etw_buffer_.shrink_to_fit();

        tree_initialized_ = true;
        PC_LOG_INFO("[TreeMgr] Tree initialized, entering normal operation");

        notify_tree_changed();

        // Start reconciliation timer (30s interval)
        schedule_reconcile();
    }

    // ---- ETW event handling ----

    void on_etw_event(const etw_process_event& evt) {
        // This runs on the strand
        auto strand_enter = std::chrono::steady_clock::now();
        auto queue_us = std::chrono::duration_cast<std::chrono::microseconds>(
            strand_enter - evt.received_at).count();

        if (!tree_initialized_) {
            // Buffer events until tree is built
            if (etw_buffer_.size() < ETW_BUFFER_LIMIT) {
                etw_buffer_.push_back(evt);
            }
            return;
        }

        apply_etw_event(evt);
        auto tree_done = std::chrono::steady_clock::now();
        notify_tree_changed();

        auto now = std::chrono::steady_clock::now();
        auto tree_us = std::chrono::duration_cast<std::chrono::microseconds>(tree_done - strand_enter).count();
        auto notify_us = std::chrono::duration_cast<std::chrono::microseconds>(now - tree_done).count();
        auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(now - evt.received_at).count();
        PC_LOG_DEBUG("[Latency] ETW {} PID={}: queue={}us tree={}us notify={}us total={}us",
                     evt.type == etw_process_event::START ? "START" : "STOP",
                     evt.pid, queue_us, tree_us, notify_us, total_us);
    }

    void apply_etw_event(const etw_process_event& evt) {
        // Runs on strand — direct flat_tree access, no locks

        if (evt.type == etw_process_event::START) {
            uint32_t idx = tree_.add_entry(evt.pid, evt.parent_pid,
                                            evt.create_time, evt.image_name);

            PC_LOG_DEBUG("[TreeMgr] ProcessStart: PID={} PPID={} {}",
                          evt.pid, evt.parent_pid, tree_.at(idx).name_u8);

            // Check auto rules for new process
            auto matched_rule = rules_.on_process_start(tree_, idx);
            if (matched_rule) {
                PC_LOG_INFO("[TreeMgr] Auto-matched: PID={} rule='{}' group={}",
                             evt.pid, *matched_rule, tree_.at(idx).group_id);
            }
        }
        else if (evt.type == etw_process_event::STOP) {
            bool ok = tree_.tombstone(evt.pid, evt.create_time);

            if (ok) {
                rules_.on_process_exit(evt.pid);
                PC_LOG_DEBUG("[TreeMgr] ProcessStop: PID={}", evt.pid);
            }
        }
    }

    // ---- Periodic reconciliation ----

    void schedule_reconcile() {
        reconcile_timer_.expires_after(std::chrono::seconds(30));
        reconcile_timer_.async_wait([this](std::error_code ec) {
            if (ec) return;  // cancelled
            do_reconcile();
        });
    }

    void do_reconcile() {
        // Run NtQuery in thread pool, then diff on strand
        asio::post(ioc_, [this]() {
            auto snapshot = ntquery_enumerate_processes();

            asio::post(strand_, [this, snap = std::move(snapshot)]() {
                reconcile_with_snapshot(snap);
                schedule_reconcile();
            });
        });
    }

    void reconcile_with_snapshot(const std::vector<raw_process_record>& snapshot) {
        // Diff snapshot vs current tree

        // Build set of PIDs in snapshot
        std::unordered_map<DWORD, const raw_process_record*> snap_map;
        for (const auto& r : snapshot)
            snap_map[r.pid] = &r;

        int added = 0, removed = 0;

        // Check for processes in snapshot but not in tree (missed START)
        for (const auto& r : snapshot) {
            uint32_t idx = tree_.find_by_pid(r.pid);
            if (idx == INVALID_IDX) {
                uint32_t new_idx = tree_.add_entry(r.pid, r.parent_pid, r.create_time, r.name);
                rules_.on_process_start(tree_, new_idx);
                added++;
            } else {
                // Check create_time match (PID reuse detection)
                const auto& entry = tree_.at(idx);
                if (entry.create_time.dwLowDateTime != r.create_time.dwLowDateTime ||
                    entry.create_time.dwHighDateTime != r.create_time.dwHighDateTime) {
                    // PID reuse: tombstone old, add new
                    tree_.tombstone_entry(idx);
                    uint32_t new_idx = tree_.add_entry(r.pid, r.parent_pid, r.create_time, r.name);
                    rules_.on_process_start(tree_, new_idx);
                    added++;
                    removed++;
                }
            }
        }

        // Check for processes in tree but not in snapshot (missed STOP)
        for (uint32_t i = 0; i < tree_.entries().size(); i++) {
            const auto& e = tree_.entries()[i];
            if (!e.alive) continue;
            if (snap_map.find(e.pid) == snap_map.end()) {
                tree_.tombstone_entry(i);
                removed++;
            }
        }

        if (added > 0 || removed > 0) {
            PC_LOG_INFO("[TreeMgr] Reconcile: +{} -{}", added, removed);
            notify_tree_changed();
        }
    }

    void notify_tree_changed() {
        if (on_tree_changed_) on_tree_changed_();
    }
};

} // namespace clew
