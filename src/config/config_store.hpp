#pragma once

// Thin wrapper around clew::config_manager. Provides the only config access
// point for HTTP handlers, application services, and cross-cutting observers
// (rule_engine sync, SSE bridge, auth_middleware, DNS manager, ...).
//
// See refactor_docs/DESIGN.md H2 + H5.
//
// Semantics:
//   - get()            : returns a full ConfigV2 copy (a few KB, microsecond).
//   - mutate(fn, tag)  : acquires the mutex, applies fn to the live ConfigV2,
//                        persists to disk, releases the mutex, then fans out
//                        observers *outside* the lock with the new snapshot
//                        and the change tag.
//   - subscribe(ob)    : register observer; called after every successful
//                        mutation. Observers must not call back into mutate
//                        synchronously (would deadlock via reentry on mutex).
//
// Threading: config_store owns its own mutex and is NOT routed through the
// process_tree_manager strand. Reason: config ops are human-driven and rare;
// routing through the strand would serialize them behind ETW events.

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/api_exception.hpp"
#include "config/config_change_tag.hpp"
#include "config/config_manager.hpp"
#include "config/types.hpp"

namespace clew {

class config_store {
public:
    using observer = std::function<void(const ConfigV2&, config_change)>;

    explicit config_store(config_manager& mgr);

    config_store(const config_store&)            = delete;
    config_store& operator=(const config_store&) = delete;

    // Returns a full ConfigV2 copy. Acquires the mutex briefly.
    [[nodiscard]] ConfigV2 get() const;

    // Raw JSON representation, for Monaco editor.
    [[nodiscard]] std::string raw_json() const;

    // Replace the entire config from a raw JSON string. Validates + persists.
    // Fires observers with config_change::wholesale_replaced.
    // Throws api_exception{invalid_argument} on parse / validation failure,
    // api_exception{io_error} on save failure.
    void replace_from_json(std::string_view raw);

    // Atomic mutation: acquire mutex, apply fn to the live ConfigV2, persist,
    // release mutex, fan out observers with (new_snapshot, tag).
    // Throws api_exception{io_error} if save() fails.
    template <typename Fn>
    void mutate(Fn&& fn, config_change tag);

    // Register observer. Subsequent mutations trigger it outside the lock.
    void subscribe(observer ob);

private:
    mutable std::mutex    mutex_;
    config_manager&       mgr_;
    std::vector<observer> observers_;
};

template <typename Fn>
void config_store::mutate(Fn&& fn, config_change tag) {
    ConfigV2 after;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::forward<Fn>(fn)(mgr_.get_v2());
        if (!mgr_.save()) {
            throw api_exception{api_error::io_error, "config save failed"};
        }
        after = mgr_.get_v2();
    }
    // Observers are called outside the lock so they may re-acquire
    // config_store via get() / raw_json() without deadlocking. Reentrant
    // mutate() still deadlocks and is considered a programming error.
    for (auto& ob : observers_) {
        ob(after, tag);
    }
}

} // namespace clew
