#include "transport/http_api_server.hpp"

#include <chrono>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "common/api_context.hpp"
#include "core/log.hpp"
#include "transport/middleware.hpp"
#include "transport/route_registry.hpp"

// Handler modules define register_* functions in the ::clew namespace.
namespace clew {
void register_config_handlers(route_registry&);
void register_connection_handlers(route_registry&);
void register_group_handlers(route_registry&);
void register_icon_handlers(route_registry&);
void register_process_handlers(route_registry&);
void register_rule_handlers(route_registry&);
void register_shell_handlers(route_registry&);
void register_sse_handlers(route_registry&);
void register_stats_handlers(route_registry&);
} // namespace clew

namespace clew::transport {

http_api_server::http_api_server(int port, api_context& ctx, std::string static_dir)
    : port_(port)
    , static_dir_(std::move(static_dir))
    , ctx_(ctx) {
    // Expand the thread pool beyond httplib's default 8 so long-lived SSE
    // connections don't starve regular requests.
    server_.new_task_queue = [] { return new httplib::ThreadPool(32); };

    install_default_headers(server_);
    install_options_handler(server_);
    install_cache_headers(server_);

    clew::route_registry reg(server_, ctx_);
    clew::register_config_handlers(reg);
    clew::register_connection_handlers(reg);
    clew::register_group_handlers(reg);
    clew::register_icon_handlers(reg);
    clew::register_process_handlers(reg);
    clew::register_rule_handlers(reg);
    clew::register_shell_handlers(reg);
    clew::register_sse_handlers(reg);
    clew::register_stats_handlers(reg);

    setup_static_files();
}

http_api_server::~http_api_server() {
    stop();
}

bool http_api_server::start() {
    if (running_.exchange(true)) return true;

    server_thread_ = std::jthread([this]() {
        PC_LOG_INFO("[api] HTTP API server listening on 127.0.0.1:{}", port_);
        if (!server_.listen("127.0.0.1", port_)) {
            PC_LOG_ERROR("[api] failed to bind port {}", port_);
            running_ = false;
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return running_;
}

void http_api_server::stop() {
    if (!running_.exchange(false)) return;
    server_.stop();
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    PC_LOG_INFO("[api] HTTP API server stopped");
}

std::string http_api_server::get_executable_dir() {
    char path[MAX_PATH];
    if (DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH); len > 0 && len < MAX_PATH) {
        std::string_view sv{path, static_cast<std::size_t>(len)};
        if (auto last_sep = sv.find_last_of("\\/"); last_sep != std::string_view::npos) {
            return std::string(sv.substr(0, last_sep));
        }
    }
    return ".";
}

void http_api_server::setup_static_files() {
    const std::vector<std::string> candidates = {
        static_dir_,
        get_executable_dir() + "/frontend/dist",
        get_executable_dir() + "/../frontend/dist",
        "./frontend/dist",
        "../frontend/dist",
        "../../frontend/dist",
        get_executable_dir() + "/ui/dist",
        "./ui/dist",
    };

    for (const auto& path : candidates) {
        if (path.empty()) continue;
        std::string index_path = path + "/index.html";
        if (GetFileAttributesA(index_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (server_.set_mount_point("/", path)) {
                PC_LOG_INFO("[api] serving static files from {}", path);
                return;
            }
        }
    }
    PC_LOG_WARN("[api] static files directory not found; web UI unavailable");
}

} // namespace clew::transport
