#include "projection/process_projection.hpp"

#include <utility>

#include <nlohmann/json.hpp>

#include "common/process_tree_json.hpp"
#include "domain/process_tree_manager.hpp"
#include "transport/sse_events.hpp"
#include "transport/sse_hub.hpp"

namespace clew {

process_projection::process_projection(domain::process_tree_manager& mgr, sse_hub& sse)
    : mgr_(mgr), sse_(sse) {
    snapshot_.store(std::make_shared<const std::string>("[]"));
}

void process_projection::on_tree_changed() {
    refresh_snapshot();
    sse_.broadcast(sse_events::process_update, nlohmann::json::object());
}

void process_projection::on_process_exit(DWORD pid) {
    // Tree snapshot was refreshed by the corresponding on_tree_changed call
    // (manager fires both when a STOP event is applied); we just need to
    // announce the exit.
    sse_.broadcast(sse_events::process_exit, nlohmann::json{{"pid", pid}});
}

std::shared_ptr<const std::string> process_projection::tree_snapshot() const noexcept {
    return snapshot_.load();
}

void process_projection::refresh_snapshot() {
    auto snap = std::make_shared<const std::string>(
        process_tree_to_json_string(mgr_.tree(), mgr_.rules()));
    snapshot_.store(std::move(snap));
}

} // namespace clew
