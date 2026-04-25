#include "services/rule_service.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "common/api_exception.hpp"
#include "common/json_patch.hpp"
#include "config/config_change_tag.hpp"
#include "config/config_store.hpp"
#include "domain/process_tree_manager.hpp"
#include "process/flat_tree.hpp"
#include "rules/rule_engine_v3.hpp"

namespace clew {

namespace {

nlohmann::json rule_entry_to_json(const AutoRule& rule, const flat_tree& tree) {
    nlohmann::json rj = rule;
    rj["matched_count"]  = rule.matched_pids.size();
    rj["excluded_count"] = rule.excluded_pids.size();
    nlohmann::json pids  = nlohmann::json::array();
    for (DWORD pid : rule.matched_pids) {
        nlohmann::json p;
        p["pid"]      = pid;
        uint32_t idx  = tree.find_by_pid(pid);
        p["name"]     = (idx != INVALID_IDX) ? std::string(tree.at(idx).name_u8) : "unknown";
        p["excluded"] = rule.excluded_pids.contains(pid);
        pids.push_back(std::move(p));
    }
    rj["matched_pids"] = std::move(pids);
    return rj;
}

} // namespace

rule_service::rule_service(strand_bound_manager& exec, config_store& cfg)
    : exec_(exec), cfg_(cfg) {}

nlohmann::json rule_service::list_rules() const {
    return exec_.query([](const domain::process_tree_manager& m) -> nlohmann::json {
        const auto& tree   = m.tree();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& rule : m.rules().auto_rules()) {
            arr.push_back(rule_entry_to_json(rule, tree));
        }
        return arr;
    });
}

void rule_service::create_rule(AutoRule rule) {
    if (rule.id.empty()) {
        rule.id = std::format("rule_{}",
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
    cfg_.mutate(
        [r = std::move(rule)](ConfigV2& c) mutable {
            c.auto_rules.push_back(std::move(r));
        },
        config_change::rule_created);
}

void rule_service::update_rule(std::string_view id, const nlohmann::json& patch) {
    bool found = false;
    cfg_.mutate(
        [&found, id, &patch](ConfigV2& c) {
            for (auto& r : c.auto_rules) {
                if (r.id == id) {
                    found = true;
                    apply_patch(r, patch,
                        field_binding{"name",               &AutoRule::name},
                        field_binding{"enabled",            &AutoRule::enabled},
                        field_binding{"process_name",       &AutoRule::process_name},
                        field_binding{"cmdline_pattern",    &AutoRule::cmdline_pattern},
                        field_binding{"image_path_pattern", &AutoRule::image_path_pattern},
                        field_binding{"hack_tree",          &AutoRule::hack_tree},
                        field_binding{"proxy_group_id",     &AutoRule::proxy_group_id},
                        field_binding{"protocol",           &AutoRule::protocol},
                        field_binding{"proxy",              &AutoRule::proxy},
                        field_binding{"dst_filter",         &AutoRule::dst_filter});
                    break;
                }
            }
        },
        config_change::rule_updated);

    if (!found) {
        throw api_exception{api_error::not_found, "Rule not found"};
    }
}

void rule_service::delete_rule(std::string_view id) {
    bool found = false;
    cfg_.mutate(
        [&found, id](ConfigV2& c) {
            auto it = std::find_if(c.auto_rules.begin(), c.auto_rules.end(),
                                   [id](const AutoRule& r) { return r.id == id; });
            if (it != c.auto_rules.end()) {
                c.auto_rules.erase(it);
                found = true;
            }
        },
        config_change::rule_deleted);

    if (!found) {
        throw api_exception{api_error::not_found, "Rule not found"};
    }
}

void rule_service::exclude_pid(std::string_view rule_id, std::uint32_t pid) {
    bool found = false;
    cfg_.mutate(
        [&found, rule_id, pid](ConfigV2& c) {
            for (auto& r : c.auto_rules) {
                if (r.id == rule_id) {
                    r.excluded_pids.insert(static_cast<DWORD>(pid));
                    found = true;
                    break;
                }
            }
        },
        config_change::rule_excluded);

    if (!found) {
        throw api_exception{api_error::not_found, "Rule not found"};
    }
}

void rule_service::unexclude_pid(std::string_view rule_id, std::uint32_t pid) {
    bool found = false;
    cfg_.mutate(
        [&found, rule_id, pid](ConfigV2& c) {
            for (auto& r : c.auto_rules) {
                if (r.id == rule_id) {
                    found = r.excluded_pids.erase(static_cast<DWORD>(pid)) > 0 || found;
                    break;
                }
            }
        },
        config_change::rule_unexcluded);

    if (!found) {
        throw api_exception{api_error::not_found, "Rule or PID not found"};
    }
}

} // namespace clew
