// /api/proxy-groups — CRUD + migrate + connectivity test.

#include <cstdint>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/api_context.hpp"
#include "services/group_service.hpp"
#include "transport/response_utils.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"

namespace clew {

namespace {

void handle_list_groups(const httplib::Request&, httplib::Response& res, const api_context& ctx) {
    write_json(res, ctx.groups.list_groups());
}

void handle_create_group(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    auto body = parse_json_body(req);
    write_json(res, ctx.groups.create_group(body));
}

void handle_update_group(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    auto id = parse_match_u32(req, 1, "id");
    auto patch = parse_json_body(req);
    ctx.groups.update_group(id, patch);
    write_json(res, nlohmann::json{{"success", true}});
}

void handle_delete_group(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    auto id = parse_match_u32(req, 1, "id");
    ctx.groups.delete_group(id);
    write_json(res, nlohmann::json{{"success", true}});
}

void handle_migrate_group(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    auto source = parse_match_u32(req, 1, "id");
    auto body = parse_json_body(req);
    std::uint32_t target = body.value("target_group_id", 0u);
    ctx.groups.migrate_group(source, target);
    write_json(res, nlohmann::json{{"success", true}});
}

void handle_test_group(const httplib::Request& req, httplib::Response& res, const api_context& ctx) {
    auto id = parse_match_u32(req, 1, "id");
    write_json(res, ctx.groups.test_group(id));
}

} // namespace

void register_group_handlers(route_registry& r) {
    using enum http_method;
    r.add({get,     "/api/proxy-groups",                &handle_list_groups});
    r.add({post,    "/api/proxy-groups",                &handle_create_group});
    r.add({put,     R"(/api/proxy-groups/(\d+))",       &handle_update_group});
    r.add({delete_, R"(/api/proxy-groups/(\d+))",       &handle_delete_group});
    r.add({post,    R"(/api/proxy-groups/(\d+)/migrate)", &handle_migrate_group});
    r.add({post,    R"(/api/proxy-groups/(\d+)/test)",  &handle_test_group});
}

} // namespace clew
