#include "services/stats_service.hpp"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "domain/process_tree_manager.hpp"

namespace clew {

stats_service::stats_service(strand_bound_manager& exec) : exec_(exec) {}

nlohmann::json stats_service::get_stats() const {
    return exec_.query([](const domain::process_tree_manager& m) -> nlohmann::json {
        nlohmann::json j;
        j["hijacked_pids"]    = m.rules().get_hijacked_pids(m.tree()).size();
        j["auto_rules_count"] = m.rules().auto_rules().size();
        return j;
    });
}

nlohmann::json stats_service::get_env() {
    auto read_env = [](const char* name) -> std::string {
        DWORD size = GetEnvironmentVariableA(name, nullptr, 0);
        if (size == 0) return {};
        std::string buf(size - 1, '\0');
        GetEnvironmentVariableA(name, buf.data(), size);
        return buf;
    };

    nlohmann::json j;
    j["HTTP_PROXY"]  = read_env("HTTP_PROXY");
    j["HTTPS_PROXY"] = read_env("HTTPS_PROXY");
    j["NO_PROXY"]    = read_env("NO_PROXY");
    j["http_proxy"]  = read_env("http_proxy");
    j["https_proxy"] = read_env("https_proxy");
    j["no_proxy"]    = read_env("no_proxy");
    return j;
}

} // namespace clew
