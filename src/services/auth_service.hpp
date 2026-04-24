#pragma once

// auth_service — thin helper for /api/bootstrap. Auth policy enforcement is
// done by auth_middleware; this service just surfaces whether auth is on.

#include <nlohmann/json.hpp>

namespace clew {

class config_store;

class auth_service {
public:
    explicit auth_service(config_store& cfg);

    auth_service(const auth_service&)            = delete;
    auth_service& operator=(const auth_service&) = delete;

    // Returns {auth_enabled: bool}. Used by unauthenticated /api/bootstrap
    // so the frontend can decide whether to prompt for a token.
    [[nodiscard]] nlohmann::json bootstrap() const;

private:
    config_store& cfg_;
};

} // namespace clew
