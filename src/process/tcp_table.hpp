#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <vector>
#include <string>
#include "core/log.hpp"

#pragma comment(lib, "iphlpapi.lib")

namespace clew {

struct tcp_connection {
    DWORD pid;
    std::string local_ip;
    uint16_t local_port;
    std::string remote_ip;
    uint16_t remote_port;
    std::string state;
};

inline std::string tcp_state_to_string(DWORD state) {
    switch (state) {
        case MIB_TCP_STATE_CLOSED: return "CLOSED";
        case MIB_TCP_STATE_LISTEN: return "LISTEN";
        case MIB_TCP_STATE_SYN_SENT: return "SYN_SENT";
        case MIB_TCP_STATE_SYN_RCVD: return "SYN_RCVD";
        case MIB_TCP_STATE_ESTAB: return "ESTABLISHED";
        case MIB_TCP_STATE_FIN_WAIT1: return "FIN_WAIT1";
        case MIB_TCP_STATE_FIN_WAIT2: return "FIN_WAIT2";
        case MIB_TCP_STATE_CLOSE_WAIT: return "CLOSE_WAIT";
        case MIB_TCP_STATE_CLOSING: return "CLOSING";
        case MIB_TCP_STATE_LAST_ACK: return "LAST_ACK";
        case MIB_TCP_STATE_TIME_WAIT: return "TIME_WAIT";
        case MIB_TCP_STATE_DELETE_TCB: return "DELETE_TCB";
        default: return "UNKNOWN";
    }
}

inline std::string ip_to_string(DWORD ip) {
    struct in_addr addr;
    addr.s_addr = ip;
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    return std::string(buf);
}

class tcp_table {
public:
    static std::vector<tcp_connection> get_connections(DWORD filter_pid = 0) {
        std::vector<tcp_connection> result;

        PMIB_TCPTABLE_OWNER_PID tcp_table = nullptr;
        DWORD size = 0;

        DWORD ret = GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                                         TCP_TABLE_OWNER_PID_ALL, 0);
        if (ret != ERROR_INSUFFICIENT_BUFFER) {
            PC_LOG_ERROR("GetExtendedTcpTable failed to get size: {}", ret);
            return result;
        }

        tcp_table = (PMIB_TCPTABLE_OWNER_PID)malloc(size);
        if (!tcp_table) {
            PC_LOG_ERROR("Failed to allocate memory for TCP table");
            return result;
        }

        ret = GetExtendedTcpTable(tcp_table, &size, FALSE, AF_INET,
                                   TCP_TABLE_OWNER_PID_ALL, 0);
        if (ret != NO_ERROR) {
            PC_LOG_ERROR("GetExtendedTcpTable failed: {}", ret);
            free(tcp_table);
            return result;
        }

        for (DWORD i = 0; i < tcp_table->dwNumEntries; i++) {
            const auto& row = tcp_table->table[i];
            if (filter_pid != 0 && row.dwOwningPid != filter_pid) continue;

            tcp_connection conn;
            conn.pid = row.dwOwningPid;
            conn.local_ip = ip_to_string(row.dwLocalAddr);
            conn.local_port = ntohs((uint16_t)row.dwLocalPort);
            conn.remote_ip = ip_to_string(row.dwRemoteAddr);
            conn.remote_port = ntohs((uint16_t)row.dwRemotePort);
            conn.state = tcp_state_to_string(row.dwState);
            result.push_back(conn);
        }

        free(tcp_table);
        return result;
    }

    static std::vector<tcp_connection> get_connections_for_pids(const std::vector<DWORD>& pids) {
        std::vector<tcp_connection> result;
        if (pids.empty()) return result;

        auto all = get_connections(0);
        for (const auto& conn : all) {
            for (DWORD pid : pids) {
                if (conn.pid == pid) {
                    result.push_back(conn);
                    break;
                }
            }
        }
        return result;
    }
};

} // namespace clew
