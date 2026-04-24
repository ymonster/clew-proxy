#pragma once

// Shared helpers used by transport/handlers/*. Keeps handlers thin:
//   parse_json_body / parse_match_u32  — input decoding (throw api_exception
//                                        on bad input)
//   write_json / write_json_text        — 200 + application/json content-type

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <nlohmann/json.hpp>

namespace httplib { struct Request; struct Response; }

namespace clew {

void write_json(httplib::Response& res, const nlohmann::json& body);
void write_json_text(httplib::Response& res, std::string_view json_text);

// Parse req.body as JSON. Throws api_exception{invalid_argument} on failure.
[[nodiscard]] nlohmann::json parse_json_body(const httplib::Request& req);

// Parse match group at index `idx` (1-based; idx=1 is the first capture)
// as unsigned 32-bit. Throws api_exception{invalid_argument} on failure.
[[nodiscard]] std::uint32_t parse_match_u32(const httplib::Request& req,
                                             std::size_t idx,
                                             std::string_view name);

} // namespace clew
