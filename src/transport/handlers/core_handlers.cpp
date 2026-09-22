// /api/subscription-core — embedded mihomo/Clash Meta core + subscription URL.

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/api_context.hpp"
#include "services/core_service.hpp"
#include "transport/response_utils.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"

namespace clew {

namespace {

void handle_get(const httplib::Request&, httplib::Response& res, const api_context& ctx) {
    write_json(res, ctx.core.status());
}

void handle_put(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    auto body = parse_json_body(req);
    write_json(res, ctx.core.update_settings(body));
}

void handle_refresh(const httplib::Request&, httplib::Response& res, const api_context& ctx) {
    write_json(res, ctx.core.refresh());
}

void handle_start(const httplib::Request&, httplib::Response& res, const api_context& ctx) {
    write_json(res, ctx.core.start());
}

void handle_stop(const httplib::Request&, httplib::Response& res, const api_context& ctx) {
    write_json(res, ctx.core.stop());
}

} // namespace

void register_core_handlers(route_registry& r) {
    using enum http_method;
    r.add({get,  "/api/subscription-core",         &handle_get});
    r.add({put,  "/api/subscription-core",         &handle_put});
    r.add({post, "/api/subscription-core/refresh", &handle_refresh});
    r.add({post, "/api/subscription-core/start",   &handle_start});
    r.add({post, "/api/subscription-core/stop",    &handle_stop});
}

} // namespace clew
