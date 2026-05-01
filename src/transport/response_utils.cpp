#include "transport/response_utils.hpp"

#include <charconv>
#include <string>

#include <httplib.h>

#include "common/api_exception.hpp"

namespace clew {

void write_json(httplib::Response& res, const nlohmann::json& body) {
    // Set status explicitly so route_registry's [api] log line records the
    // real value. cpp-httplib defaults res.status to -1 and only rewrites
    // it to 200 right before flushing, after our INFO log has already
    // captured -1. Functionally identical to the auto-200 path; just
    // makes the audit log readable.
    res.status = 200;
    res.set_content(body.dump(), "application/json");
}

void write_json_text(httplib::Response& res, std::string_view json_text) {
    res.status = 200;
    res.set_content(std::string{json_text}, "application/json");
}

nlohmann::json parse_json_body(const httplib::Request& req) {
    try {
        return nlohmann::json::parse(req.body);
    } catch (const nlohmann::json::exception& e) {
        throw api_exception{api_error::invalid_argument,
            std::string{"invalid JSON body: "} + e.what()};
    }
}

std::uint32_t parse_match_u32(const httplib::Request& req,
                              std::size_t idx,
                              std::string_view name) {
    if (idx >= req.matches.size()) {
        throw api_exception{api_error::invalid_argument,
            std::string{"missing parameter: "} + std::string{name}};
    }
    std::string s = req.matches[static_cast<int>(idx)].str();
    std::uint32_t value = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || p != s.data() + s.size()) {
        throw api_exception{api_error::invalid_argument,
            std::string{"invalid "} + std::string{name} + ": " + s};
    }
    return value;
}

} // namespace clew
