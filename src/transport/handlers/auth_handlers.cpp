// Public /api/bootstrap — used by the frontend to check whether auth is on
// before attempting authenticated requests.

#include <httplib.h>

#include "common/api_context.hpp"
#include "services/auth_service.hpp"
#include "transport/response_utils.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"

namespace clew {

namespace {

void handle_bootstrap(const httplib::Request&, httplib::Response& res, api_context& ctx) {
    write_json(res, ctx.auth.bootstrap());
}

} // namespace

void register_auth_handlers(route_registry& r) {
    r.add({http_method::get, "/api/bootstrap", auth_policy::public_endpoint, &handle_bootstrap});
}

} // namespace clew
