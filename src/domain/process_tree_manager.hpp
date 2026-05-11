#pragma once

// Strand-internal process tree + rule engine coordinator.
//
// Post-NtQuery refactor (2026-05-09):
//   - Initial tree population is driven by ETW EVENT_CONTROL_CODE_CAPTURE_STATE
//     (ProcessRundown(15)) on the same Microsoft-Windows-Kernel-Process session
//     used for real-time Start/Stop. NtQuerySystemInformation is gone — the
//     30s reconcile timer is gone too.
//   - Parent linkage uses (parent_pid, parent_psn). When the parent isn't yet
//     known we park the child in orphans_by_parent_psn_ and resolve on the
//     parent's first event arrival. Surviving orphans are flushed to root
//     1s after capture_state via rundown_grace_timer_.
//   - EventsLost from the ETW buffer callback debounces a re-issue of
//     capture_state via lost_debounce_timer_.
//
// All public methods assume the caller is already on the manager's strand;
// external code reaches them via strand_bound_manager.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include <winsock2.h>
#include <windows.h>

#include <asio.hpp>
#include <asio/steady_timer.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "config/types.hpp"                // AutoRule
#include "domain/tree_change_receiver.hpp" // push_urgency + tree_change_receiver
#include "process/etw_consumer.hpp"        // etw_consumer + etw_process_event
#include "process/flat_tree.hpp"
#include "rules/rule_engine_v3.hpp"

namespace clew {

namespace domain {

class process_tree_manager {
public:
    using strand_type = asio::strand<asio::io_context::executor_type>;

    process_tree_manager(asio::io_context& ioc, strand_type& strand);
    ~process_tree_manager() noexcept;

    process_tree_manager(const process_tree_manager&)            = delete;
    process_tree_manager& operator=(const process_tree_manager&) = delete;

    // Lifecycle (thread-safe: start may be called from any thread, internal
    // work is posted to the strand).
    void start();
    void stop() noexcept;

    // --- Mutation APIs (caller must be inside strand) ---
    bool hijack_pid(DWORD pid, bool tree_mode, uint32_t group_id);
    bool unhijack_pid(DWORD pid, bool tree_mode);

    bool batch_hijack(const std::vector<DWORD>& add,
                      const std::vector<DWORD>& remove,
                      bool tree_mode,
                      uint32_t group_id);

    bool exclude_rule_pid(std::string_view rule_id, DWORD pid);
    bool unexclude_rule_pid(std::string_view rule_id, DWORD pid);

    // --- ETW entry point (single dispatch, called on strand after ETW thread posts) ---
    void apply_etw_event_on_strand(const etw_process_event& evt);

    // --- Config sync ---
    void apply_auto_rules_from_config(const std::vector<AutoRule>& rules);

    // --- Listener registration ---
    void add_listener(tree_change_receiver* listener);

    // --- Query accessors (caller must be inside strand) ---
    [[nodiscard]] const flat_tree&       tree()  const noexcept { return tree_; }
    [[nodiscard]] flat_tree&             tree()        noexcept { return tree_; }
    [[nodiscard]] const rule_engine_v3&  rules() const noexcept { return rules_; }
    [[nodiscard]] rule_engine_v3&        rules()       noexcept { return rules_; }
    // is_initialized: true once start() has been called and rundown was
    // requested. The tree may still be filling from rundown events, but
    // HTTP API can serve from the partial state — UI startup time greatly
    // exceeds rundown processing time, so users observe a complete tree.
    [[nodiscard]] bool is_initialized() const noexcept { return started_; }

private:
    void notify_tree_changed(std::string_view source, push_urgency urgency);

    void handle_start_or_rundown(const etw_process_event& evt, bool is_rundown);
    void handle_stop(const etw_process_event& evt);
    void handle_lost(const etw_process_event& evt);
    void schedule_rundown_grace();

    // Attach a new entry to its parent given (parent_pid, parent_psn).
    // Falls back to orphans_by_parent_psn_ when the parent is unknown.
    void link_or_orphan(uint32_t new_idx, const etw_process_event& evt);

    // Drain orphans waiting for `psn` and attach them to `parent_idx`.
    void drain_orphans_for(uint64_t psn, uint32_t parent_idx);

    asio::io_context&   ioc_;
    strand_type&        strand_;
    asio::steady_timer  rundown_grace_timer_;
    asio::steady_timer  lost_debounce_timer_;

    flat_tree       tree_;
    rule_engine_v3  rules_;

    std::unique_ptr<etw_consumer> etw_;

    std::vector<tree_change_receiver*> listeners_;

    // Pending child indices keyed by their parent PSN. Drained when the
    // parent's event arrives (Start/Rundown). Whatever remains 1s after
    // initial capture_state is flushed to root by rundown_grace_timer_.
    std::unordered_map<uint64_t, std::vector<uint32_t>> orphans_by_parent_psn_;

    bool started_{false};
    bool lost_pending_{false};   // simple debounce guard
};

} // namespace domain
} // namespace clew
