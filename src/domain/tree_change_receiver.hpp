#pragma once

// Named interface replacing the old std::function<void()> callback slot on
// process_tree_manager. See refactor_docs/DESIGN.md H3.
//
// Lifetime: listener pointers are registered via process_tree_manager::add_listener
// and are expected to outlive the manager (main.cpp owns both).
// All receiver methods are invoked on the manager's strand.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>  // DWORD

namespace clew {

class tree_change_receiver {
public:
    virtual ~tree_change_receiver() = default;

    // Called after any tree mutation (ETW process start, manual hijack,
    // rule apply). Fired inside the manager's strand.
    virtual void on_tree_changed() = 0;

    // Called when a process exits (ETW ProcessStop). Fired inside the strand.
    virtual void on_process_exit(DWORD pid) = 0;
};

} // namespace clew
