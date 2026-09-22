#pragma once

// core_service — manage an embedded Clash Meta / mihomo child process fed by
// a subscription URL. Clew remains responsible for process-level routing
// (WinDivert + Rules); the core only exposes localhost SOCKS (no TUN).
//
// Owns: fetch + decode subscription, write runtime YAML, start/stop child,
// keep a ProxyGroup named "subscription" pointed at the SOCKS endpoint.
//
// Threading: methods are serialized by an internal mutex. Safe to call from
// HTTP worker threads. Does not touch asio / httplib (layering).

#include <cstdint>
#include <mutex>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

namespace clew {

class config_store;

class core_service {
public:
    explicit core_service(config_store& cfg);
    ~core_service();

    core_service(const core_service&)            = delete;
    core_service& operator=(const core_service&) = delete;

    // Snapshot for GET /api/subscription-core.
    [[nodiscard]] nlohmann::json status() const;

    // Partial update of settings (url / enabled / socks_* / core_path).
    // When enabled flips to true (or stays true) with a non-empty URL, refreshes
    // and starts. When enabled flips to false, stops the core.
    [[nodiscard]] nlohmann::json update_settings(const nlohmann::json& patch);

    // Re-download subscription, rewrite YAML, restart if enabled.
    [[nodiscard]] nlohmann::json refresh();

    [[nodiscard]] nlohmann::json start();
    [[nodiscard]] nlohmann::json stop();

    // Called from app startup: if enabled and on-disk config exists, start
    // without re-fetching. No-op when disabled.
    void try_autostart();

    // Kill child + job. Idempotent. Called from app::shutdown().
    void shutdown() noexcept;

private:
    // All require mutex_ held except where noted.
    void persist_status_fields(std::string last_error, std::string last_update);
    void ensure_proxy_group_locked();
    [[nodiscard]] std::string resolve_core_path_locked() const;
    [[nodiscard]] bool looks_like_clash_yaml(std::string_view body) const;
    [[nodiscard]] std::string decode_subscription_body(std::string_view raw) const;
    void write_runtime_files_locked(const std::string& provider_yaml);
    void stop_locked() noexcept;
    void start_locked();
    void refresh_locked();
    [[nodiscard]] nlohmann::json status_locked() const;
    [[nodiscard]] static std::string now_iso_local();

    config_store& cfg_;
    mutable std::mutex mutex_;

    HANDLE process_ = nullptr;   // child process
    HANDLE thread_  = nullptr;   // primary thread (closed after create)
    HANDLE job_     = nullptr;   // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    DWORD  pid_     = 0;
};

} // namespace clew
