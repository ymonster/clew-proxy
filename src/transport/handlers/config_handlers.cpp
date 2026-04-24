// /api/config raw-JSON GET/PUT, used by the Monaco editor.

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/api_context.hpp"
#include "services/config_service.hpp"
#include "transport/response_utils.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"

namespace clew {

namespace {

void handle_get_config(const httplib::Request&, httplib::Response& res, api_context& ctx) {
    write_json_text(res, ctx.config.get_raw());
}

void handle_put_config(const httplib::Request& req, httplib::Response& res, api_context& ctx) {
    ctx.config.replace_raw(req.body);
    write_json(res, nlohmann::json{{"success", true}});
}

} // namespace

void register_config_handlers(route_registry& r) {
    r.add({http_method::get,  "/api/config", auth_policy::required, &handle_get_config});
    r.add({http_method::put,  "/api/config", auth_policy::required, &handle_put_config});
}

} // namespace clew
