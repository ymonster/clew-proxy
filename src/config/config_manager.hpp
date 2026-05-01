#pragma once

#include <string>
#include <format>
#include <vector>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "core/log.hpp"

#include "config/types.hpp"

namespace clew {

class config_manager {
private:
    ConfigV2 v2_config_;
    std::filesystem::path config_path_;
    std::string last_raw_;
    bool has_v2_ = false;

public:
    explicit config_manager(const std::filesystem::path& path = "clew.json")
        : config_path_(path) {}

    bool load() {
        if (!std::filesystem::exists(config_path_)) {
            PC_LOG_INFO("Config file not found, using defaults");
            v2_config_ = ConfigV2{};
            has_v2_ = true;
            // ConfigV2{} default-constructs proxy_groups as empty; without
            // this call the on-disk file would also be persisted with an
            // empty proxy_groups array, leaving id=0 ("default") absent
            // until the next launch (which would re-load and run
            // ensure_proxy_groups()). Was harmless when cwd was the
            // project root (config always existed there), but became
            // visible once main.cpp pinned cwd to the exe directory.
            ensure_proxy_groups();
            return save();
        }

        try {
            std::ifstream file(config_path_);
            nlohmann::json j;
            file >> j;
            last_raw_ = j.dump(2);

            v2_config_ = j.get<ConfigV2>();
            has_v2_ = true;

            ensure_proxy_groups();

            PC_LOG_INFO("Config loaded from {}", config_path_.string());
            return true;
        } catch (const std::exception& e) {
            PC_LOG_ERROR("Failed to load config: {}", e.what());
            return false;
        }
    }

    bool save() {
        try {
            // 1. Serialize first. If nlohmann::json throws (non-UTF8 value,
            //    etc.) we bail out before touching the filesystem, so the
            //    existing file is never truncated.
            nlohmann::json j = v2_config_;
            std::string content = j.dump(2);

            // 2. Write to a temp file then rename atomically. This avoids
            //    leaving a half-written config on disk if the process
            //    crashes mid-write.
            auto tmp_path = config_path_;
            tmp_path += ".tmp";
            {
                std::ofstream file(tmp_path, std::ios::binary | std::ios::trunc);
                if (!file) {
                    PC_LOG_ERROR("Failed to open {} for writing", tmp_path.string());
                    return false;
                }
                file.write(content.data(), static_cast<std::streamsize>(content.size()));
                file.flush();
                if (!file) {
                    PC_LOG_ERROR("Failed to write {}", tmp_path.string());
                    return false;
                }
            }

            std::error_code ec;
            std::filesystem::rename(tmp_path, config_path_, ec);
            if (ec) {
                PC_LOG_ERROR("Failed to rename {} -> {}: {}",
                             tmp_path.string(), config_path_.string(), ec.message());
                std::filesystem::remove(tmp_path, ec);
                return false;
            }

            last_raw_ = std::move(content);
            PC_LOG_INFO("Config saved to {}", config_path_.string());
            return true;
        } catch (const std::exception& e) {
            PC_LOG_ERROR("Failed to save config: {}", e.what());
            return false;
        }
    }

    ConfigV2& get_v2() { return v2_config_; }
    const ConfigV2& get_v2() const { return v2_config_; }

    std::string get_raw_config() const { return last_raw_; }

    // Set raw config (from Monaco editor). Returns error message or empty on success.
    std::string set_raw_config(const std::string& json_string) {
        try {
            nlohmann::json j = nlohmann::json::parse(json_string);

            ConfigV2 test = j.get<ConfigV2>();
            if (test.version != 2) {
                return "Invalid version: expected 2";
            }

            v2_config_ = std::move(test);
            save();
            return "";
        } catch (const nlohmann::json::parse_error& e) {
            return std::string("JSON parse error: ") + e.what();
        } catch (const std::exception& e) {
            return std::string("Validation error: ") + e.what();
        }
    }

    bool reload() { return load(); }

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

        ProxyGroup default_group;
        default_group.id = 0;
        default_group.name = "default";
        default_group.host = v2_config_.default_proxy.host;
        default_group.port = v2_config_.default_proxy.port;
        default_group.type = v2_config_.default_proxy.type;
        v2_config_.proxy_groups.push_back(default_group);

        for (auto& rule : v2_config_.auto_rules) {
            if (rule.proxy.host.empty()) {
                rule.proxy_group_id = 0;
                continue;
            }

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
                ProxyGroup new_group;
                new_group.id = v2_config_.next_group_id++;
                new_group.name = std::format("group_{}", new_group.id);
                new_group.host = rule.proxy.host;
                new_group.port = rule.proxy.port;
                new_group.type = rule.proxy.type;
                rule.proxy_group_id = new_group.id;
                v2_config_.proxy_groups.push_back(new_group);
            }
        }

        PC_LOG_INFO("Migrated proxy config: {} groups created", v2_config_.proxy_groups.size());
        save();
    }
};

} // namespace clew
