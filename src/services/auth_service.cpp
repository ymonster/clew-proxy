#include "services/auth_service.hpp"

#include "config/config_store.hpp"
#include "config/types.hpp"

namespace clew {

auth_service::auth_service(config_store& cfg) : cfg_(cfg) {}

nlohmann::json auth_service::bootstrap() const {
    const auto snapshot = cfg_.get();
    nlohmann::json j;
    j["auth_enabled"] = snapshot.auth.enabled && !snapshot.auth.token.empty();
    return j;
}

} // namespace clew
