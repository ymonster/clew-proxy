#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <filesystem>
#include <optional>
#include <nlohmann/json.hpp>
#include "core/log.hpp"

#include "config/types.hpp"

namespace clew {

// v1 types (for migration)
struct proxy_settings {
    std::string socks5_host = "127.0.0.1";
    uint16_t socks5_port = 7890;
};

struct process_rule {
    bool hijack = false;
    std::vector<std::string> exclude_cidrs;
};

struct config {
    proxy_settings proxy;
    std::vector<std::string> default_exclude = {
        "127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12",
        "192.168.0.0/16", "169.254.0.0/16"
    };
    std::unordered_map<std::string, process_rule> rules;
};

class config_manager {
private:
    config v1_config_;               // v1 compat (manual rules)
    ConfigV2 v2_config_;             // v2 config
    std::filesystem::path config_path_;
    std::string last_raw_;           // Last raw JSON (for config editor)
    bool has_v2_ = false;

    // Migrate v1 config to v2
    ConfigV2 migrate_v1_to_v2(const nlohmann::json& j) {
        ConfigV2 v2;
        v2.version = 2;

        // Migrate proxy settings
        if (j.contains("proxy")) {
            v2.default_proxy.type = "socks5";
            v2.default_proxy.host = j["proxy"].value("socks5_host", "127.0.0.1");
            v2.default_proxy.port = j["proxy"].value("socks5_port", 7890);
        }

        // Migrate default excludes
        if (j.contains("default_exclude")) {
            v2.default_exclude_cidrs = j["default_exclude"].get<std::vector<std::string>>();
        }

        // Migrate process rules → auto rules
        if (j.contains("rules")) {
            for (auto& [name, rule] : j["rules"].items()) {
                bool hijack = rule.value("hijack", false);
                if (hijack) {
                    AutoRule ar;
                    ar.id = "migrated_" + name;
                    ar.name = name;
                    ar.enabled = true;
                    ar.process_name = name;
                    ar.hack_tree = false;
                    ar.proxy = v2.default_proxy;

                    // Migrate per-process excludes to dst_filter
                    if (rule.contains("exclude")) {
                        for (const auto& cidr_str : rule["exclude"]) {
                            ar.dst_filter.exclude_cidrs.push_back(CidrRange::parse(cidr_str));
                        }
                    }

                    v2.auto_rules.push_back(std::move(ar));
                }
            }
        }

        PC_LOG_INFO("Migrated v1 config to v2: {} auto rules from process rules",
                     v2.auto_rules.size());
        return v2;
    }

public:
    explicit config_manager(const std::filesystem::path& path = "clew.json")
        : config_path_(path) {}

    bool load() {
        // Try v2 config first, then fall back to v1 config.json
        if (!std::filesystem::exists(config_path_)) {
            // Try old config.json
            std::filesystem::path old_path = config_path_.parent_path() / "config.json";
            if (std::filesystem::exists(old_path)) {
                PC_LOG_INFO("Found legacy config.json, migrating to v2");
                try {
                    std::ifstream file(old_path);
                    nlohmann::json j;
                    file >> j;

                    // Also load v1 compat
                    load_v1_from_json(j);

                    v2_config_ = migrate_v1_to_v2(j);
                    has_v2_ = true;

                    // Save as v2
                    save();
                    PC_LOG_INFO("Migration complete, saved as {}", config_path_.string());
                    return true;
                } catch (const std::exception& e) {
                    PC_LOG_ERROR("Failed to migrate config: {}", e.what());
                }
            }

            PC_LOG_INFO("Config file not found, using defaults");
            v2_config_ = ConfigV2{};
            // Also init v1 compat
            v1_config_.proxy.socks5_host = v2_config_.default_proxy.host;
            v1_config_.proxy.socks5_port = v2_config_.default_proxy.port;
            has_v2_ = true;
            return save();
        }

        try {
            std::ifstream file(config_path_);
            nlohmann::json j;
            file >> j;
            last_raw_ = j.dump(2);

            int version = j.value("version", 1);
            if (version >= 2) {
                v2_config_ = j.get<ConfigV2>();
                has_v2_ = true;
            } else {
                load_v1_from_json(j);
                v2_config_ = migrate_v1_to_v2(j);
                has_v2_ = true;
                save(); // upgrade file
            }

            // Migrate proxy groups if needed
            ensure_proxy_groups();

            // Sync v1 compat from v2
            v1_config_.proxy.socks5_host = v2_config_.default_proxy.host;
            v1_config_.proxy.socks5_port = v2_config_.default_proxy.port;
            v1_config_.default_exclude = v2_config_.default_exclude_cidrs;

            PC_LOG_INFO("Config loaded from {} (v{})", config_path_.string(), version);
            return true;
        } catch (const std::exception& e) {
            PC_LOG_ERROR("Failed to load config: {}", e.what());
            return false;
        }
    }

    bool save() {
        try {
            nlohmann::json j = v2_config_;
            std::ofstream file(config_path_);
            std::string content = j.dump(2);
            file << content;
            last_raw_ = content;
            PC_LOG_INFO("Config saved to {}", config_path_.string());
            return true;
        } catch (const std::exception& e) {
            PC_LOG_ERROR("Failed to save config: {}", e.what());
            return false;
        }
    }

    // === v2 API ===
    ConfigV2& get_v2() { return v2_config_; }
    const ConfigV2& get_v2() const { return v2_config_; }

    // Raw config for Monaco editor
    std::string get_raw_config() const { return last_raw_; }

    // Set raw config (from Monaco editor). Returns error message or empty on success.
    std::string set_raw_config(const std::string& json_string) {
        try {
            nlohmann::json j = nlohmann::json::parse(json_string);

            // Validate it's a valid ConfigV2
            ConfigV2 test = j.get<ConfigV2>();
            if (test.version != 2) {
                return "Invalid version: expected 2";
            }

            v2_config_ = std::move(test);

            // Sync v1 compat
            v1_config_.proxy.socks5_host = v2_config_.default_proxy.host;
            v1_config_.proxy.socks5_port = v2_config_.default_proxy.port;
            v1_config_.default_exclude = v2_config_.default_exclude_cidrs;

            save();
            return "";
        } catch (const nlohmann::json::parse_error& e) {
            return std::string("JSON parse error: ") + e.what();
        } catch (const std::exception& e) {
            return std::string("Validation error: ") + e.what();
        }
    }

    // Hot reload
    bool reload() {
        return load();
    }

    // === v1 compat API (used by existing http_api_server routes) ===
    config& get() { return v1_config_; }
    const config& get() const { return v1_config_; }

    void set_rule(const std::string& process_name, const process_rule& rule) {
        v1_config_.rules[process_name] = rule;
    }

    std::optional<process_rule> get_rule(const std::string& process_name) const {
        auto it = v1_config_.rules.find(process_name);
        if (it != v1_config_.rules.end()) return it->second;
        return std::nullopt;
    }

    // === Proxy Group helpers ===

    const ProxyGroup* get_group_by_id(uint32_t id) const {
        for (const auto& g : v2_config_.proxy_groups) {
            if (g.id == id) return &g;
        }
        return nullptr;
    }

    const std::vector<ProxyGroup>& get_proxy_groups() const {
        return v2_config_.proxy_groups;
    }

private:
    // Ensure proxy_groups is populated (migration from default_proxy / inline proxy)
    void ensure_proxy_groups() {
        if (!v2_config_.proxy_groups.empty()) return;

        // Create default group from default_proxy
        ProxyGroup default_group;
        default_group.id = 0;
        default_group.name = "default";
        default_group.host = v2_config_.default_proxy.host;
        default_group.port = v2_config_.default_proxy.port;
        default_group.type = v2_config_.default_proxy.type;
        v2_config_.proxy_groups.push_back(default_group);

        // Migrate auto rules: find/create matching groups for inline proxy targets
        for (auto& rule : v2_config_.auto_rules) {
            if (rule.proxy.host.empty()) {
                rule.proxy_group_id = 0;  // default
                continue;
            }

            // Check if an existing group matches this proxy target
            bool found = false;
            for (const auto& g : v2_config_.proxy_groups) {
                if (g.host == rule.proxy.host && g.port == rule.proxy.port &&
                    g.type == rule.proxy.type) {
                    rule.proxy_group_id = g.id;
                    found = true;
                    break;
                }
            }

            if (!found) {
                // Create a new group for this unique proxy target
                ProxyGroup new_group;
                new_group.id = v2_config_.next_group_id++;
                new_group.name = "group_" + std::to_string(new_group.id);
                new_group.host = rule.proxy.host;
                new_group.port = rule.proxy.port;
                new_group.type = rule.proxy.type;
                rule.proxy_group_id = new_group.id;
                v2_config_.proxy_groups.push_back(new_group);
            }
        }

        PC_LOG_INFO("Migrated proxy config: {} groups created", v2_config_.proxy_groups.size());
        save();  // Persist the migration
    }

    void load_v1_from_json(const nlohmann::json& j) {
        if (j.contains("proxy")) {
            v1_config_.proxy.socks5_host = j["proxy"].value("socks5_host", "127.0.0.1");
            v1_config_.proxy.socks5_port = j["proxy"].value("socks5_port", 7890);
        }
        if (j.contains("default_exclude")) {
            v1_config_.default_exclude = j["default_exclude"].get<std::vector<std::string>>();
        }
        if (j.contains("rules")) {
            for (auto& [name, rule] : j["rules"].items()) {
                process_rule pr;
                pr.hijack = rule.value("hijack", false);
                if (rule.contains("exclude")) {
                    pr.exclude_cidrs = rule["exclude"].get<std::vector<std::string>>();
                }
                v1_config_.rules[name] = pr;
            }
        }
    }
};

} // namespace clew
