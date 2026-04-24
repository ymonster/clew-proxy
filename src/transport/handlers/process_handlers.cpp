// /api/processes*, /api/hijack*, /api/hijack/batch.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/api_context.hpp"
#include "services/process_tree_service.hpp"
#include "transport/response_utils.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"

namespace clew {

namespace {

void handle_list_processes(const httplib::Request&, httplib::Response& res, api_context& ctx) {
    auto snap = ctx.processes.tree_snapshot();
    if (!snap) {
        write_json_text(res, "[]");
        return;
    }
    write_json_text(res, *snap);
}

void handle_process_by_pid(const httplib::Request& req, httplib::Response& res, api_context& ctx) {
    auto pid = parse_match_u32(req, 1, "pid");
    write_json(res, ctx.processes.find_process(pid));
}

void handle_process_detail(const httplib::Request& req, httplib::Response& res, api_context& ctx) {
    auto pid = parse_match_u32(req, 1, "pid");
    write_json(res, ctx.processes.find_process_detail(pid));
}

void handle_list_hijacked(const httplib::Request&, httplib::Response& res, api_context& ctx) {
    write_json(res, ctx.processes.list_hijacked());
}

void handle_hijack(const httplib::Request& req, httplib::Response& res, api_context& ctx) {
    auto pid = parse_match_u32(req, 1, "pid");
    bool tree_mode = true;
    std::uint32_t gid = 0;
    if (!req.body.empty()) {
        auto body = nlohmann::json::parse(req.body, nullptr, /*allow_exceptions=*/false);
        if (!body.is_discarded()) {
            tree_mode = body.value("tree", true);
            gid       = body.value("group_id", 0u);
        }
    }
    ctx.processes.hijack(pid, tree_mode, gid);
    write_json(res, nlohmann::json{{"success", true}});
}

void handle_unhijack(const httplib::Request& req, httplib::Response& res, api_context& ctx) {
    auto pid = parse_match_u32(req, 1, "pid");
    bool tree_mode = req.has_param("tree") && req.get_param_value("tree") == "true";
    ctx.processes.unhijack(pid, tree_mode);
    write_json(res, nlohmann::json{{"success", true}});
}

void handle_batch_hijack(const httplib::Request& req, httplib::Response& res, api_context& ctx) {
    auto body = parse_json_body(req);
    auto pids = body["pids"].get<std::vector<std::uint32_t>>();
    std::string action = body.value("action", std::string{"hijack"});
    std::uint32_t gid  = body.value("group_id", 0u);
    write_json(res, ctx.processes.batch_hijack(pids, action, gid));
}

} // namespace

void register_process_handlers(route_registry& r) {
    r.add({http_method::get,     "/api/processes",                       auth_policy::required, &handle_list_processes});
    r.add({http_method::get,     R"(/api/processes/(\d+))",              auth_policy::required, &handle_process_by_pid});
    r.add({http_method::get,     R"(/api/processes/(\d+)/detail)",       auth_policy::required, &handle_process_detail});
    r.add({http_method::get,     "/api/hijack",                          auth_policy::required, &handle_list_hijacked});
    r.add({http_method::post,    R"(/api/hijack/(\d+))",                 auth_policy::required, &handle_hijack});
    r.add({http_method::delete_, R"(/api/hijack/(\d+))",                 auth_policy::required, &handle_unhijack});
    r.add({http_method::post,    "/api/hijack/batch",                    auth_policy::required, &handle_batch_hijack});
}

} // namespace clew
