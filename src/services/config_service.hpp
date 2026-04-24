#pragma once

// config_service — raw-JSON access for /api/config (Monaco editor).
// GET returns the last persisted JSON; PUT validates + persists + fires
// config_store observers (rule_engine sync, SSE bridge, auth_middleware).

#include <string>
#include <string_view>

namespace clew {

class config_store;

class config_service {
public:
    explicit config_service(config_store& cfg);

    config_service(const config_service&)            = delete;
    config_service& operator=(const config_service&) = delete;

    [[nodiscard]] std::string get_raw() const;

    // Replace entire config atomically. Throws api_exception on validation
    // failure (invalid_argument) or save failure (io_error).
    void replace_raw(std::string_view raw);

private:
    config_store& cfg_;
};

} // namespace clew
