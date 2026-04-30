#include "projection/process_projection.hpp"

#include <chrono>
#include <system_error>
#include <utility>

#include "common/process_tree_json.hpp"
#include "domain/process_tree_manager.hpp"
#include "transport/frontend_push_sink.hpp"

namespace clew {

namespace {
// Window for coalescing batched updates (ETW storms, periodic reconcile).
// Tuned by hand: short enough that human-perceptible UI lag is negligible
// (<150ms feels instant for tree updates), long enough that bursts of
// hundreds of ETW events collapse to a single push. Revisit if real-world
// data shows either edge.
constexpr auto kCoalesceWindow = std::chrono::milliseconds(100);
} // namespace

process_projection::process_projection(domain::process_tree_manager& mgr, strand_type& strand)
    : mgr_(mgr)
    , strand_(strand)
    , coalesce_timer_(strand) {
    snapshot_.store(std::make_shared<const std::string>("[]"));
}

void process_projection::on_tree_changed(push_urgency urgency) {
    refresh_snapshot();
    if (urgency == push_urgency::immediate) {
        // Cancel any pending coalesced flush and deliver right now.
        std::error_code ignore;
        coalesce_timer_.cancel(ignore);
        pending_flush_ = false;
        push_current_snapshot();
        return;
    }
    schedule_coalesced_push();
}

void process_projection::replay_to_frontend() {
    // Called from the UI thread (ready-handshake callback). Hop onto the
    // strand to refresh + push consistently.
    asio::post(strand_, [this]() {
        std::error_code ignore;
        coalesce_timer_.cancel(ignore);
        pending_flush_ = false;
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

void process_projection::schedule_coalesced_push() {
    if (pending_flush_) return;
    pending_flush_ = true;
    coalesce_timer_.expires_after(kCoalesceWindow);
    coalesce_timer_.async_wait([this](std::error_code ec) {
        // Timer was constructed from the strand executor, so this callback
        // runs on the strand — pending_flush_ access stays strand-local.
        pending_flush_ = false;
        if (ec) return;  // cancelled (e.g. an immediate push superseded us)
        push_current_snapshot();
    });
}

} // namespace clew
