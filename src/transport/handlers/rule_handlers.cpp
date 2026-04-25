// /api/auto-rules — CRUD + per-rule PID exclude.

#include <cstdint>
#include <string>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/api_context.hpp"
#include "config/types.hpp"
#include "services/rule_service.hpp"
#include "transport/response_utils.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"

namespace clew {

namespace {

void handle_list_rules(const httplib::Request&, httplib::Response& res, const api_context& ctx) {
    write_json(res, ctx.rules.list_rules());
}

void handle_create_rule(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    auto body = parse_json_body(req);
    AutoRule rule = body.get<AutoRule>();
    ctx.rules.create_rule(std::move(rule));
    write_json(res, nlohmann::json{{"success", true}});
}

void handle_update_rule(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    std::string id = req.matches[1].str();
    auto patch = parse_json_body(req);
    ctx.rules.update_rule(id, patch);
    write_json(res, nlohmann::json{{"success", true}});
}

void handle_delete_rule(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    std::string id = req.matches[1].str();
    ctx.rules.delete_rule(id);
    write_json(res, nlohmann::json{{"success", true}});
}

void handle_exclude_pid(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    std::string id = req.matches[1].str();
    auto pid = parse_match_u32(req, 2, "pid");
    ctx.rules.exclude_pid(id, pid);
    write_json(res, nlohmann::json{{"success", true}});
}

void handle_unexclude_pid(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    std::string id = req.matches[1].str();
    auto pid = parse_match_u32(req, 2, "pid");
    ctx.rules.unexclude_pid(id, pid);
    write_json(res, nlohmann::json{{"success", true}});
}

} // namespace

void register_rule_handlers(route_registry& r) {
    using enum http_method;
    r.add({get,     "/api/auto-rules",                              &handle_list_rules});
    r.add({post,    "/api/auto-rules",                              &handle_create_rule});
    r.add({put,     R"(/api/auto-rules/([^/]+))",                   &handle_update_rule});
    r.add({delete_, R"(/api/auto-rules/([^/]+))",                   &handle_delete_rule});
    r.add({post,    R"(/api/auto-rules/([^/]+)/exclude/(\d+))",     &handle_exclude_pid});
    r.add({delete_, R"(/api/auto-rules/([^/]+)/exclude/(\d+))",     &handle_unexclude_pid});
}

} // namespace clew
