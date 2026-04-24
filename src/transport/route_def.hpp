#pragma once

// Route description used by route_registry. Each handler file exports
// a register_<module>_routes(route_registry&) function that calls
// registry.add(def) for each of its routes, so the route table is
// distributed per module but assembled by the registry.

#include <string_view>

namespace httplib { struct Request; struct Response; }

namespace clew {

struct api_context;

enum class http_method { get, post, put, delete_ };
enum class auth_policy { required, public_endpoint };

using route_handler_fn = void (*)(const httplib::Request&, httplib::Response&, api_context&);

struct route_def {
    http_method       method;
    std::string_view  pattern;  // httplib-style: literal path or regex
    auth_policy       auth;
    route_handler_fn  handler;
};

} // namespace clew
