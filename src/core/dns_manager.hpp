#pragma once

// DnsManager: orchestrates DNS forwarder lifecycle + system DNS configuration.
//
// Responsibilities:
// - Start/stop dns_forwarder based on config
// - Save original system DNS to dns_state.json on enable
// - Set system DNS to listen_host (default 127.0.0.2) on active interfaces
// - Restore system DNS on disable or clean shutdown
// - Recover from previous crash (dns_state.json exists at startup)

#define ASIO_STANDALONE
#include <asio.hpp>

#include <memory>
#include <string>
#include <filesystem>

#include "config/types.hpp"
#include "core/dns_forwarder.hpp"
#include "core/system_dns.hpp"
#include "core/log.hpp"

namespace clew {

class DnsManager {
public:
    explicit DnsManager(asio::io_context& ioc,
                        std::filesystem::path state_file = "dns_state.json")
        : ioc_(ioc), state_file_(std::move(state_file)) {}

    ~DnsManager() noexcept {
        try { stop(); }
        catch (const std::exception& e) {
            PC_LOG_ERROR("[DNS-MGR] destructor caught: {}", e.what());
        } catch (...) {}
    }

    DnsManager(const DnsManager&) = delete;
    DnsManager& operator=(const DnsManager&) = delete;

    // Called once at startup before applying config: if a state file exists,
    // it means the previous session crashed while system DNS was modified.
    // Restore now and delete the file.
    void recover_crash_state() {
        auto states = system_dns::load_state(state_file_);
        if (!states) return;
        PC_LOG_INFO("[DNS-MGR] Found dns_state.json from previous session, restoring...");
        for (const auto& st : *states) {
            if (system_dns::set_interface_dns(st.adapter_guid, st.dns_servers)) {
                PC_LOG_INFO("[DNS-MGR] Restored DNS for {}: {} servers",
                             st.friendly_name, st.dns_servers.size());
            }
        }
        system_dns::delete_state(state_file_);
    }

    // Apply new config. Handles diff against current state:
    //   off → on:  start forwarder + set system DNS
    //   on → off:  stop forwarder + restore system DNS
    //   both on:   if upstream/listen changed → restart forwarder (+ re-set DNS if listen changed)
    void apply(const DnsConfig& cfg,
               const std::string& proxy_host, uint16_t proxy_port) {
        const bool was_enabled = current_cfg_.enabled;
        const bool want_enabled = cfg.enabled && cfg.mode == "forwarder";

        if (cfg.enabled && cfg.mode != "forwarder") {
            PC_LOG_WARN("[DNS-MGR] DNS mode '{}' not yet implemented (forwarder only)", cfg.mode);
        }

        if (!was_enabled && !want_enabled) {
            // still off, nothing to do
            current_cfg_ = cfg;
            return;
        }

        if (!was_enabled && want_enabled) {
            // off → on
            start_forwarder(cfg, proxy_host, proxy_port);
            set_system_dns(cfg.listen_host);
            current_cfg_ = cfg;
            return;
        }

        if (was_enabled && !want_enabled) {
            // on → off
            stop_forwarder();
            restore_system_dns();
            current_cfg_ = cfg;
            return;
        }

        // both on — check what changed
        const bool fwd_target_changed =
            current_cfg_.upstream_host != cfg.upstream_host ||
            current_cfg_.upstream_port != cfg.upstream_port ||
            current_cfg_.listen_host != cfg.listen_host ||
            current_cfg_.listen_port != cfg.listen_port ||
            current_proxy_host_ != proxy_host ||
            current_proxy_port_ != proxy_port;

        const bool listen_changed = current_cfg_.listen_host != cfg.listen_host;

        if (fwd_target_changed) {
            PC_LOG_INFO("[DNS-MGR] Config changed, restarting forwarder");
            stop_forwarder();
            start_forwarder(cfg, proxy_host, proxy_port);
        }

        if (listen_changed) {
            // System DNS now points to old listen_host, need to update
            restore_system_dns();
            set_system_dns(cfg.listen_host);
        }

        current_cfg_ = cfg;
    }

    // Clean shutdown: stop forwarder + restore system DNS + delete state file.
    void stop() {
        if (forwarder_) {
            forwarder_->stop();
            forwarder_.reset();
        }
        if (system_dns_modified_) {
            restore_system_dns();
        }
    }

private:
    asio::io_context& ioc_;
    std::filesystem::path state_file_;
    std::unique_ptr<dns_forwarder> forwarder_;
    DnsConfig current_cfg_{};
    std::string current_proxy_host_;
    uint16_t current_proxy_port_ = 0;
    bool system_dns_modified_ = false;
    std::vector<system_dns::InterfaceDnsState> saved_states_;

    void start_forwarder(const DnsConfig& cfg,
                         const std::string& proxy_host, uint16_t proxy_port) {
        forwarder_ = std::make_unique<dns_forwarder>(
            ioc_, cfg.listen_host, cfg.listen_port,
            proxy_host, proxy_port,
            cfg.upstream_host, cfg.upstream_port);
        if (forwarder_->start()) {
            PC_LOG_INFO("[DNS-MGR] Forwarder started: {}:{} -> SOCKS5 {}:{} -> {}:{}",
                         cfg.listen_host, cfg.listen_port,
                         proxy_host, proxy_port,
                         cfg.upstream_host, cfg.upstream_port);
            current_proxy_host_ = proxy_host;
            current_proxy_port_ = proxy_port;
        } else {
            PC_LOG_WARN("[DNS-MGR] Forwarder failed to start");
            forwarder_.reset();
        }
    }

    void stop_forwarder() {
        if (forwarder_) {
            forwarder_->stop();
            forwarder_.reset();
            PC_LOG_INFO("[DNS-MGR] Forwarder stopped");
        }
    }

    void set_system_dns(const std::string& listen_host) {
        auto interfaces = system_dns::enumerate_active_interfaces();
        if (interfaces.empty()) {
            PC_LOG_WARN("[DNS-MGR] No active IPv4 interfaces found, skip system DNS");
            return;
        }

        saved_states_ = interfaces;  // remember original for restore
        system_dns::save_state(state_file_, saved_states_);

        int ok = 0;
        for (const auto& st : interfaces) {
            if (system_dns::set_interface_dns(st.adapter_guid, {listen_host})) {
                ++ok;
                PC_LOG_INFO("[DNS-MGR] Set DNS {} on {} (had: {} servers)",
                             listen_host, st.friendly_name, st.dns_servers.size());
            }
        }
        system_dns_modified_ = (ok > 0);
        if (ok == 0) {
            // Nothing succeeded; no point keeping state file
            system_dns::delete_state(state_file_);
            saved_states_.clear();
        }
    }

    void restore_system_dns() {
        for (const auto& st : saved_states_) {
            if (system_dns::set_interface_dns(st.adapter_guid, st.dns_servers)) {
                PC_LOG_INFO("[DNS-MGR] Restored DNS on {}: {} servers",
                             st.friendly_name, st.dns_servers.size());
            }
        }
        system_dns::delete_state(state_file_);
        saved_states_.clear();
        system_dns_modified_ = false;
    }
};

} // namespace clew
