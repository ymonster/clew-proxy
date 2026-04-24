#include "services/group_service.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "common/api_exception.hpp"
#include "config/config_change_tag.hpp"
#include "config/config_store.hpp"
#include "config/types.hpp"
#include "core/log.hpp"
#include "domain/process_tree_manager.hpp"
#include "process/flat_tree.hpp"
#include "rules/rule_engine_v3.hpp"

namespace clew {

group_service::group_service(strand_bound_manager& exec, config_store& cfg)
    : exec_(exec), cfg_(cfg) {}

nlohmann::json group_service::list_groups() const {
    const auto cfg = cfg_.get();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& g : cfg.proxy_groups) {
        arr.push_back(nlohmann::json{
            {"id",       g.id},
            {"name",     g.name},
            {"host",     g.host},
            {"port",     g.port},
            {"type",     g.type},
            {"test_url", g.test_url},
        });
    }
    return arr;
}

nlohmann::json group_service::create_group(const nlohmann::json& body) {
    ProxyGroup group;
    cfg_.mutate(
        [&](ConfigV2& c) {
            group.id       = c.next_group_id++;
            group.name     = body.value("name", std::format("group_{}", group.id));
            group.host     = body.value("host", std::string{"127.0.0.1"});
            group.port     = body.value("port", 7890);
            group.type     = body.value("type", std::string{"socks5"});
            group.test_url = body.value("test_url", std::string{"http://www.gstatic.com/generate_204"});
            c.proxy_groups.push_back(group);
        },
        config_change::group_created);
    return group;
}

void group_service::update_group(std::uint32_t id, const nlohmann::json& patch) {
    bool found = false;
    cfg_.mutate(
        [&](ConfigV2& c) {
            for (auto& g : c.proxy_groups) {
                if (g.id == id) {
                    found = true;
                    if (patch.contains("name"))     g.name     = patch["name"];
                    if (patch.contains("host"))     g.host     = patch["host"];
                    if (patch.contains("port"))     g.port     = patch["port"];
                    if (patch.contains("type"))     g.type     = patch["type"];
                    if (patch.contains("test_url")) g.test_url = patch["test_url"];
                    break;
                }
            }
        },
        config_change::group_updated);

    if (!found) {
        throw api_exception{api_error::not_found, "Proxy group not found"};
    }
}

void group_service::delete_group(std::uint32_t id) {
    if (id == 0) {
        throw api_exception{api_error::invalid_argument, "Cannot delete the default proxy group"};
    }

    // Snapshot config for validation before mutating.
    auto snapshot = cfg_.get();

    auto it = std::find_if(snapshot.proxy_groups.begin(), snapshot.proxy_groups.end(),
                           [id](const ProxyGroup& g) { return g.id == id; });
    if (it == snapshot.proxy_groups.end()) {
        throw api_exception{api_error::not_found, "Proxy group not found"};
    }

    nlohmann::json referencing_rules = nlohmann::json::array();
    for (const auto& rule : snapshot.auto_rules) {
        if (rule.proxy_group_id == id) {
            referencing_rules.push_back(nlohmann::json{
                {"id",   rule.id},
                {"name", rule.name},
            });
        }
    }

    int manual_count = 0;
    try {
        manual_count = exec_.query([id](const domain::process_tree_manager& m) -> int {
            int count = 0;
            for (const auto& e : m.tree().entries()) {
                if (e.alive && e.group_id == id && e.has_flag(entry_flags::MANUAL_HIJACK)) {
                    ++count;
                }
            }
            return count;
        });
    } catch (const std::exception& e) {
        PC_LOG_WARN("[group_service] strand query failed during delete: {}", e.what());
    }

    if (!referencing_rules.empty() || manual_count > 0) {
        nlohmann::json details;
        details["auto_rules"]          = std::move(referencing_rules);
        details["manual_hijack_count"] = manual_count;
        throw api_exception{api_error::conflict, "group_in_use", std::move(details)};
    }

    cfg_.mutate(
        [id](ConfigV2& c) {
            std::erase_if(c.proxy_groups,
                          [id](const ProxyGroup& g) { return g.id == id; });
        },
        config_change::group_deleted);
}

void group_service::migrate_group(std::uint32_t source_id, std::uint32_t target_id) {
    if (source_id == 0) {
        throw api_exception{api_error::invalid_argument, "Cannot migrate the default group"};
    }

    auto snapshot = cfg_.get();
    bool source_ok = false;
    bool target_ok = false;
    for (const auto& g : snapshot.proxy_groups) {
        if (g.id == source_id) source_ok = true;
        if (g.id == target_id) target_ok = true;
    }
    if (!source_ok) throw api_exception{api_error::not_found, "Source group not found"};
    if (!target_ok) throw api_exception{api_error::invalid_argument, "Target group not found"};

    // Config side: rewrite rule.proxy_group_id and erase source group.
    cfg_.mutate(
        [source_id, target_id](ConfigV2& c) {
            for (auto& rule : c.auto_rules) {
                if (rule.proxy_group_id == source_id) {
                    rule.proxy_group_id = target_id;
                }
            }
            std::erase_if(c.proxy_groups,
                          [source_id](const ProxyGroup& g) { return g.id == source_id; });
        },
        config_change::group_migrated);

    // Runtime side: rewrite flat_tree entries' group_id.
    // (rule_engine auto rules get reapplied automatically by the config
    // observer sync, so no direct apply call needed here.)
    exec_.command([source_id, target_id](domain::process_tree_manager& m) {
        for (auto& e : m.tree().entries()) {
            if (e.alive && e.group_id == source_id && e.has_flag(entry_flags::MANUAL_HIJACK)) {
                e.group_id = target_id;
            }
        }
    });
}

nlohmann::json group_service::test_group(std::uint32_t id) {
    auto snapshot = cfg_.get();
    const ProxyGroup* group = nullptr;
    for (const auto& g : snapshot.proxy_groups) {
        if (g.id == id) { group = &g; break; }
    }
    if (!group) {
        throw api_exception{api_error::not_found, "Group not found"};
    }

    // Parse test_url into host + port + path. Always use port 80 (HTTPS
    // HEAD via plain socket is not valid).
    std::string test_host;
    std::string test_path = "/";
    std::uint16_t test_port = 80;
    {
        std::string url = group->test_url;
        auto pos = url.find("://");
        if (pos != std::string::npos) url = url.substr(pos + 3);
        pos = url.find('/');
        if (pos != std::string::npos) { test_path = url.substr(pos); url = url.substr(0, pos); }
        pos = url.find(':');
        if (pos != std::string::npos) {
            test_host = url.substr(0, pos);
            test_port = static_cast<std::uint16_t>(std::stoi(url.substr(pos + 1)));
        } else {
            test_host = url;
        }
    }

    auto t0 = std::chrono::steady_clock::now();

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return nlohmann::json{{"error", "socket creation failed"}};
    }
    DWORD timeout_ms = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    auto close_and_return = [&](nlohmann::json out) {
        closesocket(sock);
        return out;
    };

    sockaddr_in proxy_addr{};
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_port   = htons(group->port);
    inet_pton(AF_INET, group->host.c_str(), &proxy_addr.sin_addr);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&proxy_addr), sizeof(proxy_addr)) != 0) {
        return close_and_return(nlohmann::json{{"error", "proxy connect failed"}});
    }

    std::uint8_t greeting[] = {0x05, 0x01, 0x00};
    if (::send(sock, reinterpret_cast<const char*>(greeting), 3, 0) != 3) {
        return close_and_return(nlohmann::json{{"error", "send failed"}});
    }
    std::uint8_t greeting_reply[2] = {0};
    if (::recv(sock, reinterpret_cast<char*>(greeting_reply), 2, 0) != 2 || greeting_reply[1] != 0x00) {
        return close_and_return(nlohmann::json{{"error", "socks5 auth failed"}});
    }

    std::vector<std::uint8_t> creq = {0x05, 0x01, 0x00, 0x03,
                                      static_cast<std::uint8_t>(test_host.size())};
    creq.insert(creq.end(), test_host.begin(), test_host.end());
    creq.push_back(static_cast<std::uint8_t>(test_port >> 8));
    creq.push_back(static_cast<std::uint8_t>(test_port & 0xFF));
    if (::send(sock, reinterpret_cast<const char*>(creq.data()), static_cast<int>(creq.size()), 0)
        != static_cast<int>(creq.size())) {
        return close_and_return(nlohmann::json{{"error", "send connect failed"}});
    }
    std::uint8_t creply[10] = {0};
    int n = ::recv(sock, reinterpret_cast<char*>(creply), 10, 0);
    if (n < 4 || creply[1] != 0x00) {
        return close_and_return(nlohmann::json{{"error", "socks5 connect failed"}});
    }

    std::string head_req = "HEAD " + test_path + " HTTP/1.1\r\nHost: " + test_host
                         + "\r\nConnection: close\r\n\r\n";
    if (::send(sock, head_req.c_str(), static_cast<int>(head_req.size()), 0) <= 0) {
        return close_and_return(nlohmann::json{{"error", "send http failed"}});
    }
    char http_reply[32] = {0};
    int r = ::recv(sock, http_reply, sizeof(http_reply), 0);
    closesocket(sock);

    if (r <= 0) {
        return nlohmann::json{{"error", "timeout"}};
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    PC_LOG_DEBUG("[group_service] test group {}: {}ms to {}", id, elapsed, group->test_url);
    return nlohmann::json{{"latency_ms", elapsed}};
}

} // namespace clew
