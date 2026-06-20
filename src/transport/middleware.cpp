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
        res.set_header("Access-Control-Allow-Headers", "Content-Type");

        std::string_view path = req.path;
        if (path.starts_with("/api/")) {
            res.set_header("Cache-Control", "no-store");
        } else if (path.starts_with("/assets/")) {
            // Vite emits content-hashed filenames under /assets/. A code change
            // produces a new filename, so these can be cached forever and can
            // never serve stale code.
            res.set_header("Cache-Control", "public, max-age=31536000, immutable");
        } else {
            // index.html and SPA routes live at stable URLs ("/", "/index.html").
            // They MUST revalidate every load: cpp-httplib sends ETag +
            // Last-Modified but no Cache-Control, so WebView2 applies heuristic
            // caching and serves a stale index that references asset hashes which
            // no longer exist after a frontend rebuild (-> blank Settings / 404
            // lazy chunks). no-cache forces a conditional GET; the existing ETag
            // makes that a cheap 304 when unchanged. All localhost, so ~free.
            res.set_header("Cache-Control", "no-cache");
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
