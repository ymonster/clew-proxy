#pragma once

// Named interface replacing the old std::function<void()> callback slot on
// process_tree_manager. See refactor_docs/DESIGN.md H3.
//
// Lifetime: listener pointers are registered via process_tree_manager::add_listener
// and are expected to outlive the manager (main.cpp owns both).
// All receiver methods are invoked on the manager's strand.

namespace clew {

class tree_change_receiver {
public:
    virtual ~tree_change_receiver() = default;

    // Called after any tree mutation (ETW process start/stop, manual hijack,
    // rule apply). Fired inside the manager's strand. The previously
    // separate on_process_exit hook has been removed: a STOP already runs
    // through this same notify path, and the projection has no state to
    // update for an exit beyond the snapshot rebuild that on_tree_changed
    // already does.
    virtual void on_tree_changed() = 0;
};

} // namespace clew
