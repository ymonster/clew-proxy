// /api/shell/browse-exe + /api/shell/reveal.

#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/api_context.hpp"
#include "services/shell_service.hpp"
#include "transport/response_utils.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"

namespace clew {

namespace {

void handle_browse_exe(const httplib::Request&, httplib::Response& res, const api_context&) {
    write_json(res, shell_service::browse_exe());
}

void handle_reveal(const httplib::Request& req, httplib::Response& res, const api_context&) {
    auto body = parse_json_body(req);
    std::string path = body.value("path", std::string{});
    shell_service::reveal(path);
    write_json(res, nlohmann::json{{"success", true}});
}

} // namespace

void register_shell_handlers(route_registry& r) {
    r.add({http_method::post, "/api/shell/browse-exe", &handle_browse_exe});
    r.add({http_method::post, "/api/shell/reveal",     &handle_reveal});
}

} // namespace clew
