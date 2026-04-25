#pragma once

// Three-layer HTTP API server (replaces the legacy src/api/http_api_server
// after Stage 4 wires it in). Owns the httplib::Server + listen thread,
// installs middleware, and registers every route module on construction.
//
// Constructor arguments deliberately model the dependency graph: ctx holds
// every service, mw is pulled in for per-route auth checks, static_dir is
// the frontend/dist mount point.

#include <atomic>
#include <string>

#ifndef ASIO_STANDALONE
#define ASIO_STANDALONE
#endif

#include <thread>

#include <httplib.h>

namespace clew {
struct api_context;
}  // namespace clew

namespace clew::transport {

class http_api_server {
public:
    http_api_server(int port, api_context& ctx, std::string static_dir);
    ~http_api_server();

    http_api_server(const http_api_server&)            = delete;
    http_api_server& operator=(const http_api_server&) = delete;

    bool start();
    void stop();

    [[nodiscard]] int port() const noexcept { return port_; }

private:
    void setup_static_files();
    [[nodiscard]] static std::string get_executable_dir();

    int                 port_;
    std::string         static_dir_;
    api_context&        ctx_;
    httplib::Server     server_;
    std::jthread        server_thread_;
    std::atomic<bool>   running_{false};
};

} // namespace clew::transport
