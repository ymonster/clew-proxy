#pragma once

// process_projection — materialized tree JSON snapshot + frontend push
// fan-out for tree-change events.
//
// Registered as a tree_change_receiver on process_tree_manager in app.cpp.
// DESIGN H3: holds domain::process_tree_manager& directly (not
// strand_bound_manager) because on_tree_changed is already invoked inside
// the strand; wrapping it in strand_bound.query() here would deadlock.
//
// Coalescing: ETW storms can fire dozens of notify_tree_changed in a
// few milliseconds. Without coalescing the WebView2 V8 main thread is
// flooded with full-snapshot push messages and the UI stalls. Each
// `batched` urgency arms a steady_timer (100ms window) and a single
// push happens at the end of the window with the latest snapshot;
// `immediate` urgency cancels any pending window and pushes right away
// so user-driven actions reflect with no perceptible delay.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include <atomic>
#include <memory>
#include <string>

#include <asio.hpp>
#include <asio/steady_timer.hpp>

#include "domain/tree_change_receiver.hpp"

namespace clew {

namespace domain { class process_tree_manager; }
class frontend_push_sink;

class process_projection : public tree_change_receiver {
public:
    using strand_type = asio::strand<asio::io_context::executor_type>;

    process_projection(domain::process_tree_manager& mgr, strand_type& strand);

    ~process_projection() override = default;

    process_projection(const process_projection&)            = delete;
    process_projection& operator=(const process_projection&) = delete;

    // Wired from app.cpp once the WebView2 host (which implements the sink)
    // has been created. Setting to nullptr is safe — push() short-circuits.
    void set_sink(frontend_push_sink* sink) noexcept { sink_.store(sink); }

    // tree_change_receiver. Invoked on the strand.
    void on_tree_changed(push_urgency urgency) override;

    // Replay the most-recent snapshot to the sink. Posted to the strand
    // from the ready-handshake path on the UI thread; refreshes the
    // snapshot then pushes immediately, cancelling any pending coalesce.
    void replay_to_frontend();

private:
    void refresh_snapshot();        // strand only
    void push_current_snapshot();   // strand only
    void schedule_coalesced_push(); // strand only — arms timer if not already pending

    domain::process_tree_manager&    mgr_;
    strand_type&                     strand_;
    asio::steady_timer               coalesce_timer_;
    bool                             pending_flush_{false};

    std::atomic<frontend_push_sink*>                sink_{nullptr};
    std::atomic<std::shared_ptr<const std::string>> snapshot_;
};

} // namespace clew
