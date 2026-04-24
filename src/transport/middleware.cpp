#include "transport/middleware.hpp"

#include <string_view>

#include <httplib.h>

namespace clew {

void install_default_headers(httplib::Server& server) {
    // httplib allows only ONE post_routing_handler; combine CORS and
    // cache headers in a single lambda.
    server.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin",  "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");

        std::string_view path = req.path;
        if (path.starts_with("/api/")) {
            res.set_header("Cache-Control", "no-store");
        }
    });
}

void install_options_handler(httplib::Server& server) {
    server.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });
}

void install_cache_headers(httplib::Server& /*server*/) {
    // Intentionally no-op; cache headers live inside install_default_headers
    // because httplib only supports a single post_routing_handler.
}

} // namespace clew
