#pragma once

// Error model for all HTTP API / service boundaries.
// See refactor_docs/DESIGN.md appendix for rationale.
//
// Flow:
//   service method throws api_exception{code, message, details}
//     -> strand_bound.query/command passes it through unchanged
//     -> HTTP dispatch top-level catch maps code -> HTTP status + json body

#include <exception>
#include <string>

#include <nlohmann/json.hpp>

namespace clew {

enum class api_error {
    not_found,
    invalid_argument,
    conflict,
    unsupported,
    io_error,
    internal,
};

[[nodiscard]] int api_error_to_http_status(api_error code) noexcept;

class api_exception : public std::exception {
public:
    api_exception(api_error code, std::string message, nlohmann::json details = {});

    [[nodiscard]] api_error              code() const noexcept;
    [[nodiscard]] const std::string&     message() const noexcept;
    [[nodiscard]] const nlohmann::json&  details() const noexcept;

    const char* what() const noexcept override;

private:
    api_error       code_;
    std::string     message_;
    nlohmann::json  details_;
};

} // namespace clew
