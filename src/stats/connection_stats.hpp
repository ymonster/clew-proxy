#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include "core/log.hpp"

namespace clew {

enum class connection_status {
    connecting,
    connected,
    proxy_failed,
    closed
};

inline std::string status_to_string(connection_status s) {
    switch (s) {
        case connection_status::connecting: return "connecting";
        case connection_status::connected: return "connected";
        case connection_status::proxy_failed: return "proxy_failed";
        case connection_status::closed: return "closed";
        default: return "unknown";
    }
}

struct connection_info {
    uint64_t id;
    DWORD pid;
    std::string process_name;
    std::string dest_ip;
    uint16_t dest_port;
    connection_status status;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point closed_at;

    std::string dest_string() const {
        return dest_ip + ":" + std::to_string(dest_port);
    }
};

class connection_stats {
private:
    std::unordered_map<uint64_t, connection_info> connections_;
    std::vector<connection_info> closed_connections_;
    mutable std::mutex mutex_;
    std::atomic<uint64_t> next_id_{1};
    size_t max_closed_history_ = 1000;

public:
    uint64_t add_connection(DWORD pid, const std::string& process_name,
                           const std::string& dest_ip, uint16_t dest_port) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t id = next_id_++;
        connection_info info{};
        info.id = id;
        info.pid = pid;
        info.process_name = process_name;
        info.dest_ip = dest_ip;
        info.dest_port = dest_port;
        info.status = connection_status::connecting;
        info.bytes_sent = 0;
        info.bytes_received = 0;
        info.created_at = std::chrono::steady_clock::now();
        connections_[id] = info;
        PC_LOG_DEBUG("Connection {} added: {} -> {}", id, process_name, info.dest_string());
        return id;
    }

    void update_status(uint64_t id, connection_status status) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(id);
        if (it != connections_.end()) {
            it->second.status = status;
            if (status == connection_status::closed || status == connection_status::proxy_failed) {
                it->second.closed_at = std::chrono::steady_clock::now();
            }
        }
    }

    void update_bytes(uint64_t id, uint64_t sent, uint64_t received) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(id);
        if (it != connections_.end()) {
            it->second.bytes_sent = sent;
            it->second.bytes_received = received;
        }
    }

    void close_connection(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(id);
        if (it != connections_.end()) {
            it->second.status = connection_status::closed;
            it->second.closed_at = std::chrono::steady_clock::now();
            closed_connections_.push_back(it->second);
            if (closed_connections_.size() > max_closed_history_) {
                closed_connections_.erase(closed_connections_.begin());
            }
            connections_.erase(it);
        }
    }

    std::vector<connection_info> get_active_connections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<connection_info> result;
        for (const auto& [id, info] : connections_) {
            result.push_back(info);
        }
        return result;
    }

    std::vector<connection_info> get_connections_by_pid(DWORD pid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<connection_info> result;
        for (const auto& [id, info] : connections_) {
            if (info.pid == pid) result.push_back(info);
        }
        for (const auto& info : closed_connections_) {
            if (info.pid == pid) result.push_back(info);
        }
        return result;
    }

    std::pair<uint64_t, uint64_t> get_total_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t sent = 0, received = 0;
        for (const auto& [id, info] : connections_) {
            sent += info.bytes_sent;
            received += info.bytes_received;
        }
        return {sent, received};
    }

    size_t active_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.size();
    }
};

} // namespace clew
