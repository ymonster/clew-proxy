#pragma once

// group_service — proxy group CRUD + migrate + SOCKS5 connectivity test.
//
// CRUD mutates via config_store (source of truth). delete_group refuses
// (conflict) if any auto rule or manual hijack references the group.
// migrate_group rewrites references + erases source atomically via
// cfg_.mutate + exec_.command.

#include <cstdint>

#include <nlohmann/json.hpp>

#include "domain/strand_bound.hpp"

namespace clew {

class config_store;

class group_service {
public:
    group_service(strand_bound_manager& exec, config_store& cfg);

    group_service(const group_service&)            = delete;
    group_service& operator=(const group_service&) = delete;

    [[nodiscard]] nlohmann::json list_groups() const;

    // Auto-assigns id from next_group_id. Returns the created group as json.
    [[nodiscard]] nlohmann::json create_group(const nlohmann::json& body);

    void update_group(std::uint32_t id, const nlohmann::json& patch);

    // Throws conflict (409) if the group is referenced by any auto rule or
    // any manual hijack.
    void delete_group(std::uint32_t id);

    // Rewrites all references from source_id -> target_id, reapplies rules,
    // then erases the source. Throws if source is default (0) or target is
    // missing.
    void migrate_group(std::uint32_t source_id, std::uint32_t target_id);

    // Raw SOCKS5 + HTTP HEAD latency probe. Returns {latency_ms:...} on
    // success, {error:"..."} on failure (status 200 to match the legacy
    // frontend contract).
    [[nodiscard]] nlohmann::json test_group(std::uint32_t id);

private:
    strand_bound_manager& exec_;
    config_store&         cfg_;
};

} // namespace clew
