#pragma once

// Named interface replacing the old std::function<void()> callback slot on
// process_tree_manager. See refactor_docs/DESIGN.md H3.
//
// Lifetime: listener pointers are registered via process_tree_manager::add_listener
// and are expected to outlive the manager (main.cpp owns both).
// All receiver methods are invoked on the manager's strand.

#include <cstdint>

namespace clew {

// Push urgency hint forwarded from notify_tree_changed call sites to
// downstream listeners (projection). User-driven mutations want
// `immediate` so the UI reflects an action right away; ETW storms and
// the periodic reconcile pass use `batched` so the projection can
// coalesce many events into one push.
enum class push_urgency : std::uint8_t {
    immediate,
    batched,
};

class tree_change_receiver {
public:
    virtual ~tree_change_receiver() = default;

    // Called after any tree mutation (ETW process start/stop, manual hijack,
    // rule apply). Fired inside the manager's strand.
    virtual void on_tree_changed(push_urgency urgency) = 0;
};

} // namespace clew
