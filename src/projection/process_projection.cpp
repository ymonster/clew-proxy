#include "projection/process_projection.hpp"

#include <optional>
#include <utility>

#include <nlohmann/json.hpp>

#include "domain/process_tree_manager.hpp"
#include "process/flat_tree.hpp"
#include "rules/rule_engine_v3.hpp"
#include "transport/sse_events.hpp"
#include "transport/sse_hub.hpp"

namespace clew {

namespace {

std::string hijack_source_from_match(const std::optional<RuleMatchResult>& match) {
    if (!match) return {};
    if (match->rule_type == "manual")         return "manual";
    if (match->rule_type == "auto")           return "auto:" + match->rule_id;
    if (match->rule_type == "tree-inherited") return "tree:" + match->rule_id;
    return match->rule_type;
}

nlohmann::json entry_to_api_json(const flat_tree& tree, const rule_engine_v3& rules,
                                  uint32_t idx, bool recursive) {
    const auto& e = tree.at(idx);
    auto match = rules.get_match_info(tree, e.pid);

    nlohmann::json j;
    j["pid"]            = e.pid;
    j["parent_pid"]     = e.parent_pid;
    j["name"]           = std::string(e.name_u8);
    j["hijacked"]       = e.is_proxied();
    j["hijack_source"]  = hijack_source_from_match(match);

    if (recursive) {
        nlohmann::json children = nlohmann::json::array();
        uint32_t child = e.first_child_index;
        while (child != INVALID_IDX) {
            if (tree.at(child).alive) {
                children.push_back(entry_to_api_json(tree, rules, child, true));
            }
            child = tree.at(child).next_sibling_index;
        }
        if (!children.empty()) {
            j["children"] = std::move(children);
        }
    }
    return j;
}

std::string build_snapshot(const flat_tree& tree, const rule_engine_v3& rules) {
    nlohmann::json arr = nlohmann::json::array();
    for (uint32_t idx : tree.get_roots()) {
        arr.push_back(entry_to_api_json(tree, rules, idx, true));
    }
    return arr.dump();
}

} // namespace

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
        build_snapshot(mgr_.tree(), mgr_.rules()));
    snapshot_.store(std::move(snap));
}

} // namespace clew
