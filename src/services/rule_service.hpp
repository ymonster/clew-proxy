#pragma once

// rule_service — auto rule CRUD + runtime exclude/unexclude.
//
// CRUD operations go through config_store.mutate (source of truth); the
// config_store observer (registered in main.cpp) picks up the change and
// runs rule_engine.apply_auto_rules inside the strand, which in turn fires
// tree_change_receiver and thus SSE.
//
// list_rules reads via strand_bound_manager to see runtime state fields
// (matched_pids, excluded_pids) that are maintained in-strand.

#include <cstdint>
#include <string_view>

#include <nlohmann/json.hpp>

#include "config/types.hpp"          // AutoRule
#include "domain/strand_bound.hpp"

namespace clew {

class config_store;

class rule_service {
public:
    rule_service(strand_bound_manager& exec, config_store& cfg);

    rule_service(const rule_service&)            = delete;
    rule_service& operator=(const rule_service&) = delete;

    [[nodiscard]] nlohmann::json list_rules() const;

    // Auto-generates rule.id if empty.
    void create_rule(AutoRule rule);

    // Partial merge from json patch. Throws not_found if id is unknown.
    void update_rule(std::string_view id, const nlohmann::json& patch);

    // Throws not_found if id is unknown.
    void delete_rule(std::string_view id);

    // Runtime exclusion. Persisted via config_store so it survives reloads.
    void exclude_pid(std::string_view rule_id, std::uint32_t pid);
    void unexclude_pid(std::string_view rule_id, std::uint32_t pid);

private:
    strand_bound_manager& exec_;
    config_store&         cfg_;
};

} // namespace clew
