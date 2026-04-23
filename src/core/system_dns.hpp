#pragma once

// System DNS configuration: enumerate active IPv4 interfaces and set/restore
// their DNS servers via SetInterfaceDnsSettings (Win10 2004+).
//
// Persists original DNS state to a JSON file so that crash recovery can
// restore on next startup.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <ctime>
#include <memory>

#include "core/log.hpp"

#pragma comment(lib, "iphlpapi.lib")

namespace clew::system_dns {

struct InterfaceDnsState {
    std::string adapter_guid;          // GUID string like "{xxxx-xxxx-...}", stable across sessions
    std::string friendly_name;         // UTF-8, for logging only
    std::vector<std::string> dns_servers;  // IPv4 dotted decimal
};

// Parse "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}" into GUID
inline bool parse_guid_string(std::string_view s, GUID& out) {
    // IIDFromString wants wide string
    std::wstring w(s.begin(), s.end());
    return IIDFromString(w.c_str(), &out) == S_OK;
}

inline std::string wide_to_utf8(const wchar_t* w) {
    if (!w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

// Enumerate active IPv4 interfaces (ethernet or wifi, up, with gateway).
// Captures current DNS server list per interface.
inline std::vector<InterfaceDnsState> enumerate_active_interfaces() {
    std::vector<InterfaceDnsState> result;

    ULONG buf_len = 15000;
    auto buf = std::make_unique<char[]>(buf_len);
    auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get());

    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST
                | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_FRIENDLY_NAME;

    DWORD rc = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buf_len);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.reset(new char[buf_len]);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.get());
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &buf_len);
    }
    if (rc != NO_ERROR) {
        PC_LOG_WARN("[SYS-DNS] GetAdaptersAddresses failed: {}", rc);
        return result;
    }

    for (auto a = adapters; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        if (!a->FirstGatewayAddress) continue;  // must have IPv4 gateway

        InterfaceDnsState st;
        if (a->AdapterName) st.adapter_guid = a->AdapterName;  // e.g. "{xxxx-xxxx-...}"
        st.friendly_name = wide_to_utf8(a->FriendlyName);

        // Collect current DNS servers (IPv4 only)
        for (auto d = a->FirstDnsServerAddress; d; d = d->Next) {
            auto sa = d->Address.lpSockaddr;
            if (sa && sa->sa_family == AF_INET) {
                char ip[INET_ADDRSTRLEN] = {};
                auto sin = reinterpret_cast<sockaddr_in*>(sa);
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                st.dns_servers.emplace_back(ip);
            }
        }
        result.push_back(std::move(st));
    }
    return result;
}

// Set DNS servers for a single interface identified by its GUID string.
// Empty dns_servers list = use DHCP (reset).
inline bool set_interface_dns(const std::string& adapter_guid,
                              const std::vector<std::string>& dns_servers) {
    GUID guid{};
    if (!parse_guid_string(adapter_guid, guid)) {
        PC_LOG_WARN("[SYS-DNS] Invalid adapter GUID: {}", adapter_guid);
        return false;
    }

    // Build space-separated DNS string
    std::wstring nameserver_w;
    for (size_t i = 0; i < dns_servers.size(); ++i) {
        if (i > 0) nameserver_w.push_back(L' ');
        for (char c : dns_servers[i]) nameserver_w.push_back(static_cast<wchar_t>(c));
    }

    DNS_INTERFACE_SETTINGS settings = {};
    settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
    settings.Flags = DNS_SETTING_NAMESERVER;
    settings.NameServer = nameserver_w.empty() ? nullptr : nameserver_w.data();

    DWORD rc = SetInterfaceDnsSettings(guid, &settings);
    if (rc != NO_ERROR) {
        PC_LOG_WARN("[SYS-DNS] SetInterfaceDnsSettings failed for {}: {}",
                     adapter_guid, rc);
        return false;
    }
    return true;
}

// Persist state to JSON file (write-then-rename for atomicity).
inline bool save_state(const std::filesystem::path& file,
                       const std::vector<InterfaceDnsState>& states) {
    try {
        nlohmann::json j;
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        char buf[32];
        std::tm tm_buf{};
        gmtime_s(&tm_buf, &now);
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
        j["saved_at"] = buf;

        nlohmann::json arr = nlohmann::json::array();
        for (const auto& st : states) {
            nlohmann::json item;
            item["adapter_guid"] = st.adapter_guid;
            item["friendly_name"] = st.friendly_name;
            item["dns_servers"] = st.dns_servers;
            arr.push_back(item);
        }
        j["interfaces"] = arr;

        auto tmp = file;
        tmp += ".tmp";
        {
            std::ofstream out(tmp);
            out << j.dump(2);
        }
        std::error_code ec;
        std::filesystem::rename(tmp, file, ec);
        if (ec) {
            PC_LOG_WARN("[SYS-DNS] rename state file failed: {}", ec.message());
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        PC_LOG_WARN("[SYS-DNS] save_state exception: {}", e.what());
        return false;
    }
}

inline std::optional<std::vector<InterfaceDnsState>>
load_state(const std::filesystem::path& file) {
    try {
        std::ifstream in(file);
        if (!in) return std::nullopt;
        nlohmann::json j;
        in >> j;

        std::vector<InterfaceDnsState> out;
        for (const auto& item : j.value("interfaces", nlohmann::json::array())) {
            InterfaceDnsState st;
            st.adapter_guid = item.value("adapter_guid", std::string{});
            st.friendly_name = item.value("friendly_name", std::string{});
            st.dns_servers = item.value("dns_servers", std::vector<std::string>{});
            out.push_back(std::move(st));
        }
        return out;
    } catch (const std::exception& e) {
        PC_LOG_WARN("[SYS-DNS] load_state exception: {}", e.what());
        return std::nullopt;
    }
}

inline void delete_state(const std::filesystem::path& file) {
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

} // namespace clew::system_dns
