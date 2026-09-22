#include "services/core_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "common/api_exception.hpp"
#include "common/base64.hpp"
#include "common/http_fetch.hpp"
#include "common/json_patch.hpp"
#include "config/config_change_tag.hpp"
#include "config/config_store.hpp"
#include "config/types.hpp"
#include "core/exe_paths.hpp"
#include "core/log.hpp"
#include "core/scoped_exit.hpp"

namespace clew {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kProviderFile = "subscription_provider.yaml";
constexpr std::string_view kRuntimeFile  = "config.yaml";
constexpr std::string_view kDefaultGroup = "subscription";

fs::path core_data_dir() {
    return exe_directory() / "core_data";
}

bool write_atomic(const fs::path& path, std::string_view content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string trim_bom_and_ws(std::string s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    if (i) s.erase(0, i);
    return s;
}

bool line_has_proxies_key(std::string_view line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    constexpr std::string_view key = "proxies:";
    if (i + key.size() > line.size()) return false;
    if (line.substr(i, key.size()) != key) return false;
    // Reject proxy-providers / proxy-groups (they contain "proxies" as prefix of a longer key
    // only when written without the colon mid-token — "proxies:" is exact).
    return true;
}

} // namespace

core_service::core_service(config_store& cfg) : cfg_(cfg) {}

core_service::~core_service() {
    shutdown();
}

std::string core_service::now_iso_local() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t t = clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[64];
    // Local wall clock without zone offset — good enough for UI "last update".
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);
    return buf;
}

bool core_service::looks_like_clash_yaml(std::string_view body) const {
    // Scan line-by-line for a top-level `proxies:` key.
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('\n', start);
        if (end == std::string_view::npos) end = body.size();
        std::string_view line = body.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line_has_proxies_key(line)) return true;
        if (end == body.size()) break;
        start = end + 1;
    }
    return false;
}

std::string core_service::decode_subscription_body(std::string_view raw) const {
    std::string body = trim_bom_and_ws(std::string{raw});
    if (body.empty()) {
        throw api_exception{api_error::invalid_argument, "subscription body is empty"};
    }
    if (looks_like_clash_yaml(body)) return body;

    auto decoded = base64_decode(body);
    if (!decoded) {
        throw api_exception{api_error::invalid_argument,
            "subscription is neither Clash YAML (missing proxies:) nor valid Base64"};
    }
    std::string yaml = trim_bom_and_ws(std::move(*decoded));
    if (!looks_like_clash_yaml(yaml)) {
        throw api_exception{api_error::invalid_argument,
            "decoded subscription is not Clash YAML (missing proxies:). "
            "Share-link lists (vmess:// / ss://) are not supported in this MVP — "
            "use a Clash / mihomo YAML subscription."};
    }
    return yaml;
}

std::string core_service::resolve_core_path_locked() const {
    const auto snap = cfg_.get();
    if (!snap.subscription.core_path.empty()) {
        fs::path p{snap.subscription.core_path};
        if (fs::exists(p)) return p.string();
        throw api_exception{api_error::not_found,
            std::format("core_path not found: {}", snap.subscription.core_path)};
    }
    static constexpr const char* kCandidates[] = {
        "mihomo.exe", "clash-meta.exe", "clash.exe",
    };
    for (const char* name : kCandidates) {
        fs::path p = exe_relative(name);
        if (fs::exists(p)) return p.string();
    }
    throw api_exception{api_error::not_found,
        "No core binary found. Place mihomo.exe (or clash-meta.exe / clash.exe) "
        "next to clew.exe, or set core_path in subscription settings."};
}

void core_service::persist_status_fields(std::string last_error, std::string last_update) {
    cfg_.mutate(
        [&](ConfigV2& c) {
            c.subscription.last_error  = std::move(last_error);
            if (!last_update.empty()) {
                c.subscription.last_update = std::move(last_update);
            }
        },
        config_change::subscription_updated);
}

void core_service::ensure_proxy_group_locked() {
    const auto snap = cfg_.get();
    const auto& sc = snap.subscription;
    const std::string group_name =
        sc.group_name.empty() ? std::string{kDefaultGroup} : sc.group_name;
    const std::string host = sc.socks_host.empty() ? "127.0.0.1" : sc.socks_host;
    const uint16_t port = sc.socks_port == 0 ? 17890 : sc.socks_port;

    cfg_.mutate(
        [&](ConfigV2& c) {
            if (c.subscription.group_name.empty()) {
                c.subscription.group_name = std::string{kDefaultGroup};
            }
            ProxyGroup* found = nullptr;
            for (auto& g : c.proxy_groups) {
                if (g.name == group_name) {
                    found = &g;
                    break;
                }
            }
            if (found) {
                found->host = host;
                found->port = port;
                found->type = "socks5";
            } else {
                ProxyGroup g;
                g.id   = c.next_group_id++;
                g.name = group_name;
                g.host = host;
                g.port = port;
                g.type = "socks5";
                c.proxy_groups.push_back(std::move(g));
            }
        },
        config_change::subscription_updated);
}

void core_service::write_runtime_files_locked(const std::string& provider_yaml) {
    const auto snap = cfg_.get();
    const uint16_t port =
        snap.subscription.socks_port == 0 ? 17890 : snap.subscription.socks_port;

    const fs::path dir = core_data_dir();
    const fs::path provider_path = dir / kProviderFile;
    const fs::path runtime_path  = dir / kRuntimeFile;

    if (!write_atomic(provider_path, provider_yaml)) {
        throw api_exception{api_error::io_error, "failed to write subscription_provider.yaml"};
    }

    // Minimal mihomo/Clash Meta config: localhost SOCKS only, TUN off.
    // Provider file is referenced by relative path from -d working dir.
    const std::string runtime = std::format(
R"(# Generated by Clew — do not edit by hand.
# Localhost SOCKS only. TUN is disabled. Process filtering stays in Clew (Rules).
mixed-port: {0}
allow-lan: false
bind-address: 127.0.0.1
mode: rule
log-level: warning
ipv6: false
external-controller: ""
tun:
  enable: false
dns:
  enable: false

proxy-providers:
  subscription:
    type: file
    path: {1}
    health-check:
      enable: false
      url: http://www.gstatic.com/generate_204
      interval: 600

proxy-groups:
  - name: PROXY
    type: select
    use:
      - subscription

rules:
  - MATCH,PROXY
)",
        port, kProviderFile);

    if (!write_atomic(runtime_path, runtime)) {
        throw api_exception{api_error::io_error, "failed to write core_data/config.yaml"};
    }
    PC_LOG_INFO("[core] wrote runtime config port={} dir={}", port, dir.string());
}

void core_service::stop_locked() noexcept {
    if (process_) {
        // Prefer TerminateProcess; job object is a safety net for exit.
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, 3000);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (thread_) {
        CloseHandle(thread_);
        thread_ = nullptr;
    }
    if (job_) {
        CloseHandle(job_);  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE kills survivors
        job_ = nullptr;
    }
    pid_ = 0;
}

void core_service::start_locked() {
    stop_locked();

    const auto snap = cfg_.get();
    const fs::path runtime = core_data_dir() / kRuntimeFile;
    if (!fs::exists(runtime)) {
        throw api_exception{api_error::invalid_argument,
            "No core config on disk — click Refresh to download the subscription first"};
    }

    const std::string core = resolve_core_path_locked();
    const fs::path data_dir = core_data_dir();

    // mihomo / clash-meta / clash all accept -d <dir> -f <config>
    const std::wstring wcore = fs::path{core}.wstring();
    const std::wstring cmdline = std::format(
        L"\"{}\" -d \"{}\" -f \"{}\"",
        wcore,
        data_dir.wstring(),
        runtime.wstring());

    // CreateProcessW needs a writable buffer for the command line.
    std::wstring cmd_buf = cmdline;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    const BOOL ok = CreateProcessW(
        /*lpApplicationName*/ nullptr,
        cmd_buf.data(),
        nullptr, nullptr,
        /*bInheritHandles*/ FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr,
        data_dir.wstring().c_str(),
        &si, &pi);
    if (!ok) {
        throw api_exception{api_error::io_error,
            std::format("CreateProcessW failed (Win32 {})", GetLastError())};
    }

    // Job object: child dies when Clew exits (even on crash / TerminateProcess of Clew).
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        throw api_exception{api_error::io_error, "CreateJobObjectW failed"};
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) ||
        !AssignProcessToJobObject(job, pi.hProcess)) {
        const DWORD err = GetLastError();
        CloseHandle(job);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        throw api_exception{api_error::io_error,
            std::format("AssignProcessToJobObject failed (Win32 {})", err)};
    }

    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        const DWORD err = GetLastError();
        CloseHandle(job);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        throw api_exception{api_error::io_error,
            std::format("ResumeThread failed (Win32 {})", err)};
    }

    process_ = pi.hProcess;
    thread_  = pi.hThread;
    job_     = job;
    pid_     = pi.dwProcessId;

    ensure_proxy_group_locked();
    PC_LOG_INFO("[core] started pid={} path={}", pid_, core);

    // Brief settle — if the process exits immediately, surface the failure.
    if (WaitForSingleObject(process_, 400) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        stop_locked();
        throw api_exception{api_error::io_error,
            std::format("core exited immediately (code {}). Check core_data/ and the binary.", code)};
    }
}

void core_service::refresh_locked() {
    const auto snap = cfg_.get();
    if (snap.subscription.url.empty()) {
        throw api_exception{api_error::invalid_argument, "subscription URL is empty"};
    }

    PC_LOG_INFO("[core] fetching subscription...");
    auto resp = http_get(snap.subscription.url);
    if (!resp.error.empty()) {
        persist_status_fields(resp.error, {});
        throw api_exception{api_error::io_error,
            std::format("subscription fetch failed: {}", resp.error)};
    }

    std::string yaml;
    try {
        yaml = decode_subscription_body(resp.body);
    } catch (const api_exception& e) {
        persist_status_fields(e.message(), {});
        throw;
    }

    // Also keep the raw body for debugging (best-effort).
    write_atomic(core_data_dir() / "subscription_raw.txt", resp.body);

    try {
        write_runtime_files_locked(yaml);
        ensure_proxy_group_locked();
    } catch (const api_exception& e) {
        persist_status_fields(e.message(), {});
        throw;
    }

    persist_status_fields(/*last_error*/ "", now_iso_local());

    if (snap.subscription.enabled || process_ != nullptr) {
        // Re-read enabled after persist (settings may have changed); prefer current.
        const bool want_run = cfg_.get().subscription.enabled || process_ != nullptr;
        if (want_run) {
            try {
                start_locked();
            } catch (const api_exception& e) {
                persist_status_fields(e.message(), {});
                throw;
            }
        }
    }
}

nlohmann::json core_service::status_locked() const {
    const auto snap = cfg_.get();
    const auto& sc = snap.subscription;

    bool running = false;
    if (process_) {
        const DWORD wait = WaitForSingleObject(process_, 0);
        running = (wait == WAIT_TIMEOUT);
    }

    std::string resolved;
    try {
        // const_cast path: resolve may throw; swallow for status display.
        resolved = resolve_core_path_locked();
    } catch (...) {
        resolved = "";
    }

    std::uint32_t group_id = 0;
    bool group_found = false;
    const std::string gname =
        sc.group_name.empty() ? std::string{kDefaultGroup} : sc.group_name;
    for (const auto& g : snap.proxy_groups) {
        if (g.name == gname) {
            group_id = g.id;
            group_found = true;
            break;
        }
    }

    return nlohmann::json{
        {"enabled",            sc.enabled},
        {"url",                sc.url},
        {"socks_host",         sc.socks_host.empty() ? "127.0.0.1" : sc.socks_host},
        {"socks_port",         sc.socks_port == 0 ? 17890 : sc.socks_port},
        {"core_path",          sc.core_path},
        {"resolved_core_path", resolved},
        {"running",            running},
        {"pid",                running ? static_cast<std::uint32_t>(pid_) : 0},
        {"last_error",         sc.last_error},
        {"last_update",        sc.last_update},
        {"group_name",         gname},
        {"group_id",           group_found ? nlohmann::json(group_id) : nlohmann::json(nullptr)},
        {"data_dir",           core_data_dir().string()},
        {"config_path",        (core_data_dir() / kRuntimeFile).string()},
    };
}

nlohmann::json core_service::status() const {
    std::scoped_lock lock(mutex_);
    return status_locked();
}

nlohmann::json core_service::update_settings(const nlohmann::json& patch) {
    std::scoped_lock lock(mutex_);

    bool enable_requested = false;
    bool disable_requested = false;
    bool had_enabled = cfg_.get().subscription.enabled;

    cfg_.mutate(
        [&](ConfigV2& c) {
            apply_patch(c.subscription, patch,
                field_binding{"enabled",    &SubscriptionCoreConfig::enabled},
                field_binding{"url",        &SubscriptionCoreConfig::url},
                field_binding{"socks_host", &SubscriptionCoreConfig::socks_host},
                field_binding{"socks_port", &SubscriptionCoreConfig::socks_port},
                field_binding{"core_path",  &SubscriptionCoreConfig::core_path},
                field_binding{"group_name", &SubscriptionCoreConfig::group_name});
            if (c.subscription.socks_host.empty()) {
                c.subscription.socks_host = "127.0.0.1";
            }
            if (c.subscription.socks_port == 0) {
                c.subscription.socks_port = 17890;
            }
            if (c.subscription.group_name.empty()) {
                c.subscription.group_name = std::string{kDefaultGroup};
            }
        },
        config_change::subscription_updated);

    const bool now_enabled = cfg_.get().subscription.enabled;
    if (now_enabled && !had_enabled) enable_requested = true;
    if (!now_enabled && had_enabled) disable_requested = true;
    // Also treat explicit enabled:true in patch as enable intent when already on
    // (user may have changed URL and wants restart).
    if (patch.contains("enabled") && patch["enabled"].is_boolean() &&
        patch["enabled"].get<bool>()) {
        enable_requested = true;
    }

    if (disable_requested) {
        stop_locked();
        persist_status_fields("", {});
        return status_locked();
    }

    // Keep proxy group host/port in sync with settings even when stopped.
    ensure_proxy_group_locked();

    if (enable_requested) {
        const auto snap = cfg_.get();
        if (snap.subscription.url.empty()) {
            persist_status_fields("enabled but URL is empty", {});
            return status_locked();
        }
        try {
            refresh_locked();
        } catch (const api_exception&) {
            // Error already persisted; return status so UI can show last_error.
            return status_locked();
        }
    } else if (process_ && (patch.contains("socks_port") || patch.contains("socks_host") ||
                            patch.contains("core_path"))) {
        // Port / binary changed while running — rewrite + restart if we have provider.
        const fs::path provider = core_data_dir() / kProviderFile;
        if (fs::exists(provider)) {
            std::ifstream in(provider, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            write_runtime_files_locked(ss.str());
            try {
                start_locked();
            } catch (const api_exception& e) {
                persist_status_fields(e.message(), {});
            }
        }
    }

    return status_locked();
}

nlohmann::json core_service::refresh() {
    std::scoped_lock lock(mutex_);
    try {
        refresh_locked();
    } catch (const api_exception&) {
        return status_locked();
    }
    return status_locked();
}

nlohmann::json core_service::start() {
    std::scoped_lock lock(mutex_);
    try {
        // Ensure enabled flag is on when user explicitly starts.
        cfg_.mutate(
            [](ConfigV2& c) { c.subscription.enabled = true; },
            config_change::subscription_updated);
        if (!fs::exists(core_data_dir() / kRuntimeFile)) {
            refresh_locked();
        } else {
            start_locked();
            persist_status_fields("", {});
        }
    } catch (const api_exception& e) {
        persist_status_fields(e.message(), {});
    }
    return status_locked();
}

nlohmann::json core_service::stop() {
    std::scoped_lock lock(mutex_);
    stop_locked();
    cfg_.mutate(
        [](ConfigV2& c) { c.subscription.enabled = false; },
        config_change::subscription_updated);
    return status_locked();
}

void core_service::try_autostart() {
    std::scoped_lock lock(mutex_);
    const auto snap = cfg_.get();
    if (!snap.subscription.enabled) return;
    if (!fs::exists(core_data_dir() / kRuntimeFile)) {
        PC_LOG_WARN("[core] enabled but no config.yaml — waiting for Refresh");
        return;
    }
    try {
        start_locked();
        PC_LOG_INFO("[core] autostart OK");
    } catch (const api_exception& e) {
        PC_LOG_WARN("[core] autostart failed: {}", e.message());
        persist_status_fields(e.message(), {});
    }
}

void core_service::shutdown() noexcept {
    std::scoped_lock lock(mutex_);
    stop_locked();
}

} // namespace clew
