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

struct udp_endpoint {
    DWORD pid;
    std::string local_ip;
    uint16_t local_port;
};

class udp_table {
public:
    static std::vector<udp_endpoint> get_endpoints(DWORD filter_pid = 0) {
        std::vector<udp_endpoint> result;

        PMIB_UDPTABLE_OWNER_PID table = nullptr;
        DWORD size = 0;

        DWORD ret = GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET,
                                         UDP_TABLE_OWNER_PID, 0);
        if (ret != ERROR_INSUFFICIENT_BUFFER) {
            PC_LOG_ERROR("GetExtendedUdpTable failed to get size: {}", ret);
            return result;
        }

        table = (PMIB_UDPTABLE_OWNER_PID)malloc(size);
        if (!table) {
            PC_LOG_ERROR("Failed to allocate memory for UDP table");
            return result;
        }

        ret = GetExtendedUdpTable(table, &size, FALSE, AF_INET,
                                   UDP_TABLE_OWNER_PID, 0);
        if (ret != NO_ERROR) {
            PC_LOG_ERROR("GetExtendedUdpTable failed: {}", ret);
            free(table);
            return result;
        }

        for (DWORD i = 0; i < table->dwNumEntries; i++) {
            const auto& row = table->table[i];
            if (filter_pid != 0 && row.dwOwningPid != filter_pid) continue;

            udp_endpoint ep;
            ep.pid = row.dwOwningPid;
            ep.local_ip = ip_to_string(row.dwLocalAddr);
            ep.local_port = ntohs((uint16_t)row.dwLocalPort);
            result.push_back(ep);
        }

        free(table);
        return result;
    }
};

} // namespace clew
