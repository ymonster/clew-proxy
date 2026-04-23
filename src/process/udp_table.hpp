#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <cstddef>
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
        DWORD size = 0;

        DWORD ret = GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET,
                                         UDP_TABLE_OWNER_PID, 0);
        if (ret != ERROR_INSUFFICIENT_BUFFER) {
            PC_LOG_ERROR("GetExtendedUdpTable failed to get size: {}", ret);
            return result;
        }

        // RAII backing storage for the variable-size MIB_UDPTABLE_OWNER_PID.
        std::vector<std::byte> buf(size);
        auto* table = reinterpret_cast<PMIB_UDPTABLE_OWNER_PID>(buf.data());

        ret = GetExtendedUdpTable(table, &size, FALSE, AF_INET,
                                   UDP_TABLE_OWNER_PID, 0);
        if (ret != NO_ERROR) {
            PC_LOG_ERROR("GetExtendedUdpTable failed: {}", ret);
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

        return result;
    }
};

} // namespace clew
