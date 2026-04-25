#pragma once

// Global cross-cutting behaviors installed on the httplib::Server once.
// Per-route auth is handled inside route_registry.dispatch, not here.

namespace httplib { class Server; }

namespace clew {

// Adds CORS headers to every response.
void install_default_headers(httplib::Server& server);

// Answers any OPTIONS request with 204 + CORS headers (from install_default_headers).
void install_options_handler(httplib::Server& server);

// Sets Cache-Control: no-store for /api responses; static mounts stay default.
void install_cache_headers(httplib::Server& server);

} // namespace clew
