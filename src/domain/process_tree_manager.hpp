#pragma once

// Strand-internal process tree + rule engine coordinator.
//
// Redesigned wrapper over flat_tree + rule_engine_v3 + etw_consumer +
// NtQuery reconcile timer. All public methods assume the caller is already
// on the manager's strand; external code reaches them via strand_bound_manager.
//
// See refactor_docs/DESIGN.md H3 (notify ownership) and H5 (batch-merge).
//
// Lives under clew::domain for module isolation.

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
#include <vector>

#include "config/types.hpp"              // AutoRule
#include "process/etw_consumer.hpp"      // etw_consumer + etw_process_event
#include "process/flat_tree.hpp"
#include "process/ntquery_snapshot.hpp"  // raw_process_record + ntquery_enumerate_processes
#include "rules/rule_engine_v3.hpp"

namespace clew {

class tree_change_receiver;

namespace domain {

class process_tree_manager {
public:
    using strand_type = asio::strand<asio::io_context::executor_type>;

    // Does NOT own the strand; caller (main.cpp) owns both io_context and strand.
    process_tree_manager(asio::io_context& ioc, strand_type& strand);
    ~process_tree_manager() noexcept;

    process_tree_manager(const process_tree_manager&)            = delete;
    process_tree_manager& operator=(const process_tree_manager&) = delete;

    // Lifecycle (thread-safe: start may be called from any thread, internal
    // work is posted to the strand).
    void start();
    void stop() noexcept;

    // --- Mutation APIs (caller must be inside strand) ---
    // Return true if the mutation actually changed state (e.g. pid found and
    // hijack applied). Callers can distinguish 404 from 200-ok-noop.
    bool hijack_pid(DWORD pid, bool tree_mode, uint32_t group_id);
    bool unhijack_pid(DWORD pid, bool tree_mode);

    // Apply a diff atomically: every add/remove processed, exactly ONE
    // notify_tree_changed fired at the end. This is the H5 correctness fix
    // relative to the legacy server which would emit N SSE events.
    // tree_mode=true applies hijack to the full subtree; tree_mode=false
    // is a single-pid hijack (matches the legacy /api/hijack/batch semantics).
    bool batch_hijack(const std::vector<DWORD>& add,
                      const std::vector<DWORD>& remove,
                      bool tree_mode,
                      uint32_t group_id);

    bool exclude_rule_pid(std::string_view rule_id, DWORD pid);
    bool unexclude_rule_pid(std::string_view rule_id, DWORD pid);

    // --- ETW entry points (called on strand after ETW thread posts) ---
    void on_etw_process_start(const etw_process_event& evt);
    void on_etw_process_stop(const etw_process_event& evt);

    // --- Config sync (called via strand_bound_manager.command from main's
    //     config_store observer) ---
    void apply_auto_rules_from_config(const std::vector<AutoRule>& rules);

    // --- Listener registration ---
    // listener must outlive the manager. There is intentionally no
    // remove_listener — lifetime is coordinated by main.cpp which owns both.
    void add_listener(tree_change_receiver* listener);

    // --- Query accessors (caller must be inside strand) ---
    [[nodiscard]] const flat_tree&       tree()  const noexcept { return tree_; }
    [[nodiscard]] flat_tree&             tree()        noexcept { return tree_; }
    [[nodiscard]] const rule_engine_v3&  rules() const noexcept { return rules_; }
    [[nodiscard]] rule_engine_v3&        rules()       noexcept { return rules_; }
    [[nodiscard]] bool is_initialized() const noexcept { return tree_initialized_; }

private:
    // `source` is a short literal label ("etw_start" / "etw_stop" /
    // "manual_hijack" / "manual_unhijack" / "batch_hijack" / "rule_exclude" /
    // "rule_unexclude" / "auto_rules_apply" / "init" / "reconcile") used by
    // the [DIAG-NOTIFY] log line so we can attribute SSE broadcast pressure
    // to its origin. Caller must pass a string with static lifetime (literal).
    void notify_tree_changed(std::string_view source);

    void apply_etw_event_locked(const etw_process_event& evt);
    void build_initial_tree(const std::vector<raw_process_record>& snapshot);
    void schedule_reconcile();
    void reconcile_with_snapshot(const std::vector<raw_process_record>& snapshot);

    asio::io_context&   ioc_;
    strand_type&        strand_;
    asio::steady_timer  reconcile_timer_;

    flat_tree       tree_;
    rule_engine_v3  rules_;

    std::unique_ptr<etw_consumer> etw_;

    std::vector<tree_change_receiver*> listeners_;

    bool started_{false};
    bool tree_initialized_{false};

    static constexpr std::size_t kEtwBufferLimit = 2048;
    std::vector<etw_process_event> etw_buffer_;
};

} // namespace domain
} // namespace clew
