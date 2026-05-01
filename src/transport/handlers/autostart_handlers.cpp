// /api/autostart — get/set the Windows Task Scheduler "ClewAutoStart"
// task that launches clew on user logon. State lives in Task Scheduler,
// NOT in clew.json — it is OS-level configuration and we always read it
// fresh so manual edits via taskschd.msc reflect immediately in the UI.

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/api_exception.hpp"
#include "services/autostart_service.hpp"
#include "transport/response_utils.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"

namespace clew {

namespace {

void handle_get_autostart(const httplib::Request&, httplib::Response& res, const api_context&) {
    write_json(res, to_json(autostart_service::query()));
}

void handle_put_autostart(const httplib::Request& req, httplib::Response& res, const api_context&) {
    auto body = parse_json_body(req);
    if (!body.contains("enabled") || !body["enabled"].is_boolean()) {
        throw api_exception{api_error::invalid_argument, "missing bool field 'enabled'"};
    }
    bool enabled         = body["enabled"].get<bool>();
    bool start_minimized = body.value("start_minimized", false);

    autostart_service::set(enabled, start_minimized);
    // Re-query so the response reflects the actual on-disk state, in case
    // schtasks coerced anything (e.g. existing task with different flags).
    write_json(res, to_json(autostart_service::query()));
}

} // namespace

void register_autostart_handlers(route_registry& r) {
    using enum http_method;
    r.add({get, "/api/autostart", &handle_get_autostart});
    r.add({put, "/api/autostart", &handle_put_autostart});
}

} // namespace clew
