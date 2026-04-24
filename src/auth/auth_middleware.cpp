#include "auth/auth_middleware.hpp"

#include <string_view>

#include <httplib.h>

#include "config/config_change_tag.hpp"
#include "config/config_store.hpp"
#include "config/types.hpp"

namespace clew {

auth_middleware::auth_middleware(config_store& cfg) {
    refresh_from_config(cfg.get());
    cfg.subscribe([this](const ConfigV2& c, config_change) {
        refresh_from_config(c);
    });
}

void auth_middleware::refresh_from_config(const ConfigV2& cfg) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    enabled_ = cfg.auth.enabled && !cfg.auth.token.empty();
    token_   = cfg.auth.token;
}

bool auth_middleware::verify(const httplib::Request& req) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (!enabled_) return true;

    std::string_view header = req.get_header_value("Authorization");
    constexpr std::string_view prefix = "Bearer ";
    if (header.size() <= prefix.size() || header.substr(0, prefix.size()) != prefix) {
        return false;
    }
    return header.substr(prefix.size()) == token_;
}

} // namespace clew
