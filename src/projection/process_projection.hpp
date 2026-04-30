#pragma once

// process_projection — materialized tree JSON snapshot + SSE fan-out for
// tree change / process exit events.
//
// Registered as a tree_change_receiver on process_tree_manager in main.cpp.
// DESIGN H3: holds domain::process_tree_manager& directly (not
// strand_bound_manager) because on_tree_changed is already invoked inside
// the strand; wrapping it in strand_bound.query() here would deadlock.

#include <atomic>
#include <memory>
#include <string>

#include "domain/tree_change_receiver.hpp"

namespace clew {

namespace domain { class process_tree_manager; }
class sse_hub;

class process_projection : public tree_change_receiver {
public:
    process_projection(domain::process_tree_manager& mgr, sse_hub& sse);

    ~process_projection() override = default;

    process_projection(const process_projection&)            = delete;
    process_projection& operator=(const process_projection&) = delete;

    // tree_change_receiver
    void on_tree_changed() override;

    // Lock-free read for process_tree_service.
    [[nodiscard]] std::shared_ptr<const std::string> tree_snapshot() const noexcept;

private:
    void refresh_snapshot();   // called inside strand

    domain::process_tree_manager& mgr_;
    sse_hub&                      sse_;
    std::atomic<std::shared_ptr<const std::string>> snapshot_;
};

} // namespace clew
