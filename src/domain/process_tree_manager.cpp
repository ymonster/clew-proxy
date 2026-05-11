#include "domain/process_tree_manager.hpp"

#include <chrono>
#include <system_error>
#include <utility>

#include "core/log.hpp"
#include "domain/tree_change_receiver.hpp"

namespace clew::domain {

process_tree_manager::process_tree_manager(asio::io_context& ioc, strand_type& strand)
    : ioc_(ioc)
    , strand_(strand)
    , rundown_grace_timer_(ioc)
    , lost_debounce_timer_(ioc) {}

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

    PC_LOG_INFO("[TreeMgr] Starting (ETW+capture_state, no NtQuery)...");

    // ETW consumer posts every event onto the strand. The DTO carries
    // {kind, pid, psn, parent_pid, parent_psn, create_time, exit_time,
    //  image_name, lost_count} — no separate START/STOP entry points.
    etw_ = std::make_unique<etw_consumer>([this](const etw_process_event& evt) {
        asio::post(strand_, [this, evt]() mutable {
            apply_etw_event_on_strand(evt);
        });
    });
    if (!etw_->start()) {
        PC_LOG_ERROR("[TreeMgr] ETW consumer failed to start");
        return;
    }

    // Initial population. capture_state runs synchronously on the
    // ETW kernel side, then ProcessRundown(15) events stream in via the
    // same callback path as real-time Start/Stop.
    asio::post(strand_, [this]() {
        etw_->request_rundown();
        schedule_rundown_grace();
    });
}

void process_tree_manager::stop() noexcept {
    if (!started_) return;
    started_ = false;

    std::error_code ec;
    rundown_grace_timer_.cancel(ec);
    lost_debounce_timer_.cancel(ec);
    if (etw_) etw_->stop();

    PC_LOG_INFO("[TreeMgr] Stopped");
}

void process_tree_manager::add_listener(tree_change_receiver* listener) {
    if (listener) listeners_.push_back(listener);
}

void process_tree_manager::notify_tree_changed(std::string_view source, push_urgency urgency) {
    PC_LOG_DEBUG("[tree-change] source={}", source);
    for (auto* l : listeners_) l->on_tree_changed(urgency);
}

// --- Mutation APIs ---

bool process_tree_manager::hijack_pid(DWORD pid, bool tree_mode, uint32_t gid) {
    if (tree_.find_by_pid(pid) == INVALID_IDX) return false;
    if (tree_mode) {
        rules_.manual_hijack_tree(tree_, pid, gid);
    } else {
        rules_.manual_hijack(tree_, pid, gid);
    }
    notify_tree_changed("manual_hijack", push_urgency::immediate);
    return true;
}

bool process_tree_manager::unhijack_pid(DWORD pid, bool tree_mode) {
    if (tree_.find_by_pid(pid) == INVALID_IDX) return false;
    if (tree_mode) {
        rules_.manual_unhijack_tree(tree_, pid);
    } else {
        rules_.manual_unhijack(tree_, pid);
    }
    notify_tree_changed("manual_unhijack", push_urgency::immediate);
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

    if (changes > 0) notify_tree_changed("batch_hijack", push_urgency::immediate);
    return changes > 0;
}

bool process_tree_manager::exclude_rule_pid(std::string_view rule_id, DWORD pid) {
    bool ok = rules_.exclude_pid(tree_, rule_id, pid);
    if (ok) notify_tree_changed("rule_exclude", push_urgency::immediate);
    return ok;
}

bool process_tree_manager::unexclude_rule_pid(std::string_view rule_id, DWORD pid) {
    bool ok = rules_.unexclude_pid(rule_id, pid);
    if (ok) notify_tree_changed("rule_unexclude", push_urgency::immediate);
    return ok;
}

void process_tree_manager::apply_auto_rules_from_config(const std::vector<AutoRule>& rules) {
    rules_.set_auto_rules(rules);
    rules_.apply_auto_rules(tree_);
    notify_tree_changed("auto_rules_apply", push_urgency::immediate);
}

// --- ETW dispatch ---

void process_tree_manager::apply_etw_event_on_strand(const etw_process_event& evt) {
    switch (evt.kind) {
        case etw_process_event_kind::START:
            handle_start_or_rundown(evt, /*is_rundown=*/false);
            break;
        case etw_process_event_kind::RUNDOWN:
            handle_start_or_rundown(evt, /*is_rundown=*/true);
            break;
        case etw_process_event_kind::STOP:
            handle_stop(evt);
            break;
        case etw_process_event_kind::EVENTS_LOST:
            handle_lost(evt);
            break;
    }
}

void process_tree_manager::handle_start_or_rundown(const etw_process_event& evt, bool is_rundown) {
    // 1. Idempotency: same (pid, psn) already known -> skip. Common during
    //    capture_state (rundown of a process whose Start we already saw)
    //    and after lost+recovery cycles.
    if (tree_.find_by_pid_psn(evt.pid, evt.psn) != INVALID_IDX) {
        return;
    }

    // 2. add_entry handles PID-reuse internally: if pid exists with a
    //    different PSN, it tombstones the old before installing new.
    uint32_t new_idx = tree_.add_entry(evt.pid, evt.parent_pid,
                                       evt.psn, evt.parent_psn,
                                       evt.create_time,
                                       evt.image_name.c_str());

    // 3. Parent linkage.
    link_or_orphan(new_idx, evt);

    // 4. Activate any orphans waiting for me as their parent.
    drain_orphans_for(evt.psn, new_idx);

    // 5. Rule engine + push.
    auto matched = rules_.on_process_start(tree_, new_idx);
    if (matched) {
        PC_LOG_INFO("[TreeMgr] Auto-matched: PID={} PSN={} rule='{}' group={}",
                     evt.pid, evt.psn, *matched, tree_.at(new_idx).group_id);
    }
    notify_tree_changed(is_rundown ? "etw_rundown" : "etw_start",
                        is_rundown ? push_urgency::batched : push_urgency::immediate);
}

void process_tree_manager::handle_stop(const etw_process_event& evt) {
    // PSN verification: stale STOPs (after PID was recycled) silently
    // ignored — find_by_pid_psn returns INVALID_IDX when psn doesn't match.
    uint32_t idx = tree_.find_by_pid_psn(evt.pid, evt.psn);
    if (idx == INVALID_IDX) return;

    bool ok = tree_.tombstone(evt.pid, evt.psn);
    if (ok) {
        rules_.on_process_exit(evt.pid);
        notify_tree_changed("etw_stop", push_urgency::batched);
    }
}

void process_tree_manager::handle_lost(const etw_process_event& evt) {
    PC_LOG_WARN("[TreeMgr] ETW EventsLost +{} cumulative={}",
                 evt.lost_count, evt.lost_cumulative);
    if (lost_pending_) return;  // already armed
    lost_pending_ = true;

    lost_debounce_timer_.expires_after(std::chrono::seconds(1));
    lost_debounce_timer_.async_wait([this](std::error_code ec) {
        lost_pending_ = false;
        if (ec) return;
        if (!started_) return;
        PC_LOG_INFO("[TreeMgr] Re-requesting capture_state after lost burst");
        if (etw_) etw_->request_rundown();
    });
}

void process_tree_manager::link_or_orphan(uint32_t new_idx, const etw_process_event& evt) {
    if (evt.parent_psn == ROOT_PSN_SENTINEL || evt.parent_psn == INVALID_PSN) {
        tree_.mark_root(new_idx);
        return;
    }
    uint32_t parent_idx = tree_.find_by_pid_psn(evt.parent_pid, evt.parent_psn);
    if (parent_idx != INVALID_IDX) {
        tree_.attach_child(parent_idx, new_idx);
        return;
    }
    // Parent absent: park as orphan keyed by parent_psn.
    orphans_by_parent_psn_[evt.parent_psn].push_back(new_idx);
}

void process_tree_manager::drain_orphans_for(uint64_t psn, uint32_t parent_idx) {
    auto it = orphans_by_parent_psn_.find(psn);
    if (it == orphans_by_parent_psn_.end()) return;
    for (uint32_t child_idx : it->second) {
        // Defensive: child may have been tombstoned in the interim
        // (extremely unlikely but possible if Stop arrives before
        // Start of the parent in some edge race).
        if (child_idx >= tree_.entries().size()) continue;
        if (!tree_.entries()[child_idx].alive) continue;
        tree_.attach_child(parent_idx, child_idx);
    }
    orphans_by_parent_psn_.erase(it);
}

void process_tree_manager::schedule_rundown_grace() {
    // capture_state issuance returns immediately, but the kernel takes ~2s
    // to enumerate live processes and emit ProcessRundown events. The grace
    // timer must be long enough to outlast that delay so any cross-event
    // ordering quirks (child before parent inside the rundown stream) get
    // resolved before we flush surviving orphans to root. 5s is comfortably
    // above the observed 2s envelope on a typical box.
    rundown_grace_timer_.expires_after(std::chrono::seconds(5));
    rundown_grace_timer_.async_wait([this](std::error_code ec) {
        if (ec) return;
        if (!started_) return;

        // Anything still in orphans_ post-rundown means the parent was
        // lost (process died between rundown enumeration and our event,
        // or its Start was dropped pre-startup). Flush to root so the
        // tree remains reachable from get_roots().
        std::size_t flushed = 0;
        for (auto& [parent_psn, children] : orphans_by_parent_psn_) {
            for (uint32_t child_idx : children) {
                if (child_idx >= tree_.entries().size()) continue;
                if (!tree_.entries()[child_idx].alive) continue;
                tree_.mark_root(child_idx);
                ++flushed;
            }
        }
        orphans_by_parent_psn_.clear();
        if (flushed > 0) {
            PC_LOG_INFO("[TreeMgr] Rundown grace: {} orphans flushed to root", flushed);
            notify_tree_changed("rundown_grace", push_urgency::batched);
        } else {
            PC_LOG_INFO("[TreeMgr] Rundown grace: no orphans (clean tree, {} alive)",
                         tree_.alive_count());
        }
    });
}

} // namespace clew::domain
