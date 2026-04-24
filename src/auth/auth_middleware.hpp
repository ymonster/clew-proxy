#pragma once

// auth_middleware — cross-cutting bearer-token check installed on every
// non-public route by the transport layer.
//
// The token is read from ConfigV2.auth on construction and refreshed
// in-place via a config_store observer, so UI token updates take effect
// without restarting the server. shared_mutex lets any number of request
// threads read concurrently while the observer writes.

#include <shared_mutex>
#include <string>

namespace httplib {
struct Request;
}

namespace clew {

class config_store;
struct ConfigV2;

class auth_middleware {
public:
    explicit auth_middleware(config_store& cfg);

    auth_middleware(const auth_middleware&)            = delete;
    auth_middleware& operator=(const auth_middleware&) = delete;

    // Returns true if the request carries a valid token (or auth is off).
    [[nodiscard]] bool verify(const httplib::Request& req) const;

private:
    void refresh_from_config(const ConfigV2& cfg);

    mutable std::shared_mutex mu_;
    bool        enabled_{false};
    std::string token_;
};

} // namespace clew
