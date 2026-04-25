// /api/tcp + /api/udp: OS connection tables enriched with hijack status.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <httplib.h>

#include "common/api_context.hpp"
#include "common/api_exception.hpp"
#include "services/connection_service.hpp"
#include "transport/response_utils.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"

namespace clew {

namespace {

std::optional<std::uint32_t> pid_from_query(const httplib::Request& req) {
    if (!req.has_param("pid")) return std::nullopt;
    try {
        return static_cast<std::uint32_t>(std::stoul(req.get_param_value("pid")));
    } catch (const std::exception&) {
        throw api_exception{api_error::invalid_argument, "invalid pid parameter"};
    }
}

void handle_tcp(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    write_json(res, ctx.connections.list_tcp(pid_from_query(req)));
}

void handle_udp(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    write_json(res, ctx.connections.list_udp(pid_from_query(req)));
}

} // namespace

void register_connection_handlers(route_registry& r) {
    r.add({http_method::get, "/api/tcp", &handle_tcp});
    r.add({http_method::get, "/api/udp", &handle_udp});
}

} // namespace clew
