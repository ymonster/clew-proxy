#include "common/api_exception.hpp"

#include <utility>

namespace clew {

api_exception::api_exception(api_error code, std::string message, nlohmann::json details)
    : code_(code)
    , message_(std::move(message))
    , details_(std::move(details)) {}

api_error             api_exception::code()    const noexcept { return code_; }
const std::string&    api_exception::message() const noexcept { return message_; }
const nlohmann::json& api_exception::details() const noexcept { return details_; }
const char*           api_exception::what()    const noexcept { return message_.c_str(); }

int api_error_to_http_status(api_error code) noexcept {
    switch (code) {
        case api_error::not_found:        return 404;
        case api_error::invalid_argument: return 400;
        case api_error::conflict:         return 409;
        case api_error::auth_failed:      return 401;
        case api_error::forbidden:        return 403;
        case api_error::unsupported:      return 501;
        case api_error::io_error:         return 500;
        case api_error::internal:         return 500;
    }
    return 500;
}

} // namespace clew
