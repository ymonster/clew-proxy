#pragma once

// stats_service — aggregate runtime numbers for /api/stats and /api/env.
//
// get_stats() reads the process tree + rule engine via strand_bound_manager.
// get_env() is a pure function (no state) reading HTTP_PROXY-family env vars.

#include <nlohmann/json.hpp>

#include "domain/strand_bound.hpp"

namespace clew {

class stats_service {
public:
    explicit stats_service(strand_bound_manager& exec);

    stats_service(const stats_service&)            = delete;
    stats_service& operator=(const stats_service&) = delete;

    // Counts of hijacked PIDs + active auto rules, plus future stats.
    [[nodiscard]] nlohmann::json get_stats() const;

    // HTTP_PROXY / HTTPS_PROXY / NO_PROXY (upper + lower case) snapshot.
    [[nodiscard]] static nlohmann::json get_env();

private:
    strand_bound_manager& exec_;
};

} // namespace clew
