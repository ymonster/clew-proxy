#pragma once

// route_registry — accepts route_def descriptors and installs them on a
// httplib::Server. Each adapter lambda runs DESIGN's three-tier dispatch:
// auth check -> handler -> catch(api_exception) / std::exception / (...).

#include "transport/route_def.hpp"

namespace httplib { class Server; }

namespace clew {

struct api_context;
class auth_middleware;

class route_registry {
public:
    route_registry(httplib::Server& server, api_context& ctx, auth_middleware& mw);

    route_registry(const route_registry&)            = delete;
    route_registry& operator=(const route_registry&) = delete;

    void add(const route_def& def);

private:
    httplib::Server&  server_;
    api_context&      ctx_;
    auth_middleware&  mw_;
};

} // namespace clew
