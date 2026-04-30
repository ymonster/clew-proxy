#include "domain/process_tree_manager.hpp"

#include <chrono>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "core/log.hpp"
#include "domain/tree_change_receiver.hpp"

namespace clew::domain {

process_tree_manager::process_tree_manager(asio::io_context& ioc, strand_type& strand)
    : ioc_(ioc)
    , strand_(strand)
    , reconcile_timer_(ioc) {}

process_tree_manager::~process_tree_manager() noexcept {
    try {
        stop();
    } catch (const std::exception& e) {
        PC_LOG_ERROR("[TreeMgr] destructor caught: {}", e.what());
    } catch (...) {
        PC_LOG_ERROR("[TreeMgr] destructor caught non-std exception");
    }
}

void process_tree_manager::start() {
    if (started_) return;
    started_ = true;

    PC_LOG_INFO("[TreeMgr] Starting initialization sequence...");

    // t0: Start ETW consumer — events buffered until the initial tree is built.
    etw_ = std::make_unique<etw_consumer>([this](const etw_process_event& evt) {
        asio::post(strand_, [this, evt]() mutable {
            if (evt.type == etw_process_event::START) {
                on_etw_process_start(evt);
            } else {
                on_etw_process_stop(evt);
            }
        });
    });
    if (!etw_->start()) {
        PC_LOG_ERROR("[TreeMgr] ETW consumer failed to start");
        return;
    }

    // t1: NtQuery initial snapshot off-strand, t2: build tree on strand.
    asio::post(ioc_, [this]() {
        auto snap = ntquery_enumerate_processes();
        asio::post(strand_, [this, snap = std::move(snap)]() {
            build_initial_tree(snap);
        });
    });
}

void process_tree_manager::stop() noexcept {
    if (!started_) return;
    started_ = false;

    std::error_code ec;
    reconcile_timer_.cancel(ec);
    if (etw_) etw_->stop();

    PC_LOG_INFO("[TreeMgr] Stopped");
}

void process_tree_manager::add_listener(tree_change_receiver* listener) {
    if (listener) listeners_.push_back(listener);
}

void process_tree_manager::notify_tree_changed(std::string_view source) {
    PC_LOG_INFO("[DIAG-NOTIFY] source={} listeners={}", source, listeners_.size());
    for (auto* l : listeners_) l->on_tree_changed();
}

// --- Mutation APIs ---

bool process_tree_manager::hijack_pid(DWORD pid, bool tree_mode, uint32_t gid) {
    if (tree_.find_by_pid(pid) == INVALID_IDX) return false;
    if (tree_mode) {
        rules_.manual_hijack_tree(tree_, pid, gid);
    } else {
        rules_.manual_hijack(tree_, pid, gid);
    }
    notify_tree_changed("manual_hijack");
    return true;
}

bool process_tree_manager::unhijack_pid(DWORD pid, bool tree_mode) {
    if (tree_.find_by_pid(pid) == INVALID_IDX) return false;
    if (tree_mode) {
        rules_.manual_unhijack_tree(tree_, pid);
    } else {
        rules_.manual_unhijack(tree_, pid);
    }
    notify_tree_changed("manual_unhijack");
    return true;
}

bool process_tree_manager::batch_hijack(const std::vector<DWORD>& add,
                                        const std::vector<DWORD>& remove,
                                        bool tree_mode,
                                        uint32_t gid) {
    std::size_t changes = 0;

    for (auto pid : add) {
        if (tree_.find_by_pid(pid) != INVALID_IDX) {
            if (tree_mode) rules_.manual_hijack_tree(tree_, pid, gid);
            else           rules_.manual_hijack(tree_, pid, gid);
            ++changes;
        }
    }
    for (auto pid : remove) {
        if (tree_.find_by_pid(pid) != INVALID_IDX) {
            if (tree_mode) rules_.manual_unhijack_tree(tree_, pid);
            else           rules_.manual_unhijack(tree_, pid);
            ++changes;
        }
    }

    if (changes > 0) notify_tree_changed("batch_hijack");
    return changes > 0;
}

bool process_tree_manager::exclude_rule_pid(std::string_view rule_id, DWORD pid) {
    bool ok = rules_.exclude_pid(tree_, rule_id, pid);
    if (ok) notify_tree_changed("rule_exclude");
    return ok;
}

bool process_tree_manager::unexclude_rule_pid(std::string_view rule_id, DWORD pid) {
    bool ok = rules_.unexclude_pid(rule_id, pid);
    if (ok) notify_tree_changed("rule_unexclude");
    return ok;
}

void process_tree_manager::apply_auto_rules_from_config(const std::vector<AutoRule>& rules) {
    rules_.set_auto_rules(rules);
    rules_.apply_auto_rules(tree_);
    notify_tree_changed("auto_rules_apply");
}

// --- ETW entry points ---

void process_tree_manager::on_etw_process_start(const etw_process_event& evt) {
    if (!tree_initialized_) {
        if (etw_buffer_.size() < kEtwBufferLimit) etw_buffer_.push_back(evt);
        return;
    }
    apply_etw_event_locked(evt);
    notify_tree_changed("etw_start");
}

void process_tree_manager::on_etw_process_stop(const etw_process_event& evt) {
    if (!tree_initialized_) {
        if (etw_buffer_.size() < kEtwBufferLimit) etw_buffer_.push_back(evt);
        return;
    }
    apply_etw_event_locked(evt);
    notify_tree_changed("etw_stop");
}

// --- Internal event handling ---

void process_tree_manager::apply_etw_event_locked(const etw_process_event& evt) {
    if (evt.type == etw_process_event::START) {
        uint32_t idx = tree_.add_entry(evt.pid, evt.parent_pid, evt.create_time, evt.image_name);
        auto matched = rules_.on_process_start(tree_, idx);
        if (matched) {
            PC_LOG_INFO("[TreeMgr] Auto-matched: PID={} rule='{}' group={}",
                         evt.pid, *matched, tree_.at(idx).group_id);
        }
    } else if (evt.type == etw_process_event::STOP) {
        bool ok = tree_.tombstone(evt.pid, evt.create_time);
        if (ok) {
            // Rule-engine internal cleanup (drops PID from matched_pids /
            // excluded_pids). Distinct from the retired SSE process_exit
            // event; the outer on_etw_process_stop() already fires
            // notify_tree_changed("etw_stop") which is the sole signal.
            rules_.on_process_exit(evt.pid);
        }
    }
}

void process_tree_manager::build_initial_tree(const std::vector<raw_process_record>& snapshot) {
    tree_.build_from_snapshot(snapshot);
    PC_LOG_INFO("[TreeMgr] Initial tree built: {} processes", tree_.alive_count());

    std::size_t replayed = 0;
    for (const auto& evt : etw_buffer_) {
        apply_etw_event_locked(evt);
        ++replayed;
    }
    if (replayed > 0) {
        PC_LOG_INFO("[TreeMgr] Replayed {} buffered ETW events", replayed);
    }
    etw_buffer_.clear();
    etw_buffer_.shrink_to_fit();

    tree_initialized_ = true;
    PC_LOG_INFO("[TreeMgr] Tree initialized, entering normal operation");

    notify_tree_changed("init");
    schedule_reconcile();
}

void process_tree_manager::schedule_reconcile() {
    reconcile_timer_.expires_after(std::chrono::seconds(30));
    reconcile_timer_.async_wait([this](std::error_code ec) {
        if (ec) return;  // cancelled (stop)
        asio::post(ioc_, [this] {
            auto snap = ntquery_enumerate_processes();
            asio::post(strand_, [this, snap = std::move(snap)]() {
                reconcile_with_snapshot(snap);
                schedule_reconcile();
            });
        });
    });
}

void process_tree_manager::reconcile_with_snapshot(const std::vector<raw_process_record>& snapshot) {
    std::unordered_map<DWORD, const raw_process_record*> snap_map;
    for (const auto& r : snapshot) snap_map[r.pid] = &r;

    int added = 0;
    int removed = 0;

    // Processes in snapshot but not in tree (missed START).
    for (const auto& r : snapshot) {
        uint32_t idx = tree_.find_by_pid(r.pid);
        if (idx == INVALID_IDX) {
            uint32_t new_idx = tree_.add_entry(r.pid, r.parent_pid, r.create_time, r.name);
            rules_.on_process_start(tree_, new_idx);
            ++added;
        } else {
            const auto& entry = tree_.at(idx);
            if (entry.create_time.dwLowDateTime  != r.create_time.dwLowDateTime ||
                entry.create_time.dwHighDateTime != r.create_time.dwHighDateTime) {
                // PID reuse — tombstone old, add new.
                tree_.tombstone_entry(idx);
                uint32_t new_idx = tree_.add_entry(r.pid, r.parent_pid, r.create_time, r.name);
                rules_.on_process_start(tree_, new_idx);
                ++added;
                ++removed;
            }
        }
    }

    // Processes in tree but not in snapshot (missed STOP).
    for (uint32_t i = 0; i < tree_.entries().size(); ++i) {
        const auto& e = tree_.entries()[i];
        if (!e.alive) continue;
        if (!snap_map.contains(e.pid)) {
            tree_.tombstone_entry(i);
            ++removed;
        }
    }

    if (added > 0 || removed > 0) {
        PC_LOG_INFO("[TreeMgr] Reconcile: +{} -{}", added, removed);
        notify_tree_changed("reconcile");
    }
}

} // namespace clew::domain
