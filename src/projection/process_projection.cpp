#include "projection/process_projection.hpp"

#include <chrono>
#include <system_error>
#include <utility>

#include "common/process_tree_json.hpp"
#include "domain/process_tree_manager.hpp"
#include "transport/frontend_push_sink.hpp"

namespace clew {

#ifdef CLEW_PROJECTION_COALESCE
namespace {
// Window for coalescing batched updates (ETW storms).
// Trade-off: tree updates can lag up to this much; chosen so the lag is
// sub-perceptible (<150ms feels instant) while collapsing bursts of
// hundreds of ETW events into a single refresh+push.
constexpr auto kCoalesceWindow = std::chrono::milliseconds(100);
} // namespace
#endif

process_projection::process_projection(domain::process_tree_manager& mgr, strand_type& strand)
    : mgr_(mgr)
    , strand_(strand)
#ifdef CLEW_PROJECTION_COALESCE
    , coalesce_timer_(strand)
#endif
{
    snapshot_.store(std::make_shared<const std::string>("[]"));
}

void process_projection::on_tree_changed(push_urgency urgency) {
    if (!frontend_visible_.load(std::memory_order_relaxed)) {
        // Frontend hidden (window minimized or in tray). Skip both
        // refresh and push — replay_to_frontend (triggered by the ready
        // handshake on visible) refreshes on demand and delivers the
        // latest snapshot.
        return;
    }
#ifndef CLEW_PROJECTION_COALESCE
    // Default build: every event refresh + push synchronously. Snappy,
    // and the strand cost is negligible at typical loads (refresh is
    // O(N) but ~2us per process; ETW rate is typically <30/sec).
    (void)urgency;
    refresh_snapshot();
    push_current_snapshot();
#else
    if (urgency == push_urgency::immediate) {
        // User-driven action — bypass the timer regardless.
        std::error_code ignore;
        coalesce_timer_.cancel(ignore);
        pending_flush_ = false;
        refresh_snapshot();
        push_current_snapshot();
        return;
    }
    // Batched (ETW): just arm/keep the timer. The timer callback runs
    // refresh+push together at the end of the 100ms window so a burst
    // of hundreds of events collapses to a single snapshot.
    schedule_coalesced_push();
#endif
}

void process_projection::replay_to_frontend() {
    // Called from the UI thread (ready-handshake callback). Hop onto the
    // strand to refresh + push consistently.
    asio::post(strand_, [this]() {
#ifdef CLEW_PROJECTION_COALESCE
        std::error_code ignore;
        coalesce_timer_.cancel(ignore);
        pending_flush_ = false;
#endif
        refresh_snapshot();
        push_current_snapshot();
    });
}

void process_projection::refresh_snapshot() {
    auto snap = std::make_shared<const std::string>(
        process_tree_to_json_string(mgr_.tree(), mgr_.rules()));
    snapshot_.store(std::move(snap));
}

void process_projection::push_current_snapshot() {
    auto* sink = sink_.load();
    if (!sink) return;
    auto snap = snapshot_.load();
    if (!snap) return;
    sink->push("process_update", *snap);
}

#ifdef CLEW_PROJECTION_COALESCE
void process_projection::schedule_coalesced_push() {
    if (pending_flush_) return;
    pending_flush_ = true;
    coalesce_timer_.expires_after(kCoalesceWindow);
    coalesce_timer_.async_wait([this](std::error_code ec) {
        // Timer was constructed from the strand executor, so this
        // callback runs on the strand — pending_flush_ access stays
        // strand-local.
        pending_flush_ = false;
        if (ec) return;  // cancelled (e.g. an immediate push superseded us)
        refresh_snapshot();
        push_current_snapshot();
    });
}
#endif

} // namespace clew
