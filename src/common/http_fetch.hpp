#pragma once

// Synchronous HTTP(S) GET via WinHTTP. Used by core_service so services/
// never include httplib (layering guard in scripts/verify.sh). Supports
// HTTPS through the system certificate store — no OpenSSL link required.

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include "core/scoped_exit.hpp"

#pragma comment(lib, "winhttp.lib")

namespace clew {

struct http_fetch_result {
    int         status = 0;
    std::string body;
    std::string error;   // non-empty on transport / parse failure
};

namespace detail {

inline std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

} // namespace detail

// GET `url`. Follows redirects (WinHTTP default). Timeout ~30s.
[[nodiscard]] inline http_fetch_result http_get(std::string_view url,
                                                std::string_view user_agent = "Clew/subscription-core") {
    http_fetch_result out;
    if (url.empty()) {
        out.error = "empty URL";
        return out;
    }

    const std::wstring wurl = detail::utf8_to_wide(url);
    if (wurl.empty()) {
        out.error = "URL is not valid UTF-8";
        return out;
    }

    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength    = static_cast<DWORD>(-1);
    uc.dwHostNameLength  = static_cast<DWORD>(-1);
    uc.dwUrlPathLength   = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc)) {
        out.error = "WinHttpCrackUrl failed";
        return out;
    }

    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength > 0 && uc.lpszExtraInfo) {
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";

    HINTERNET session = WinHttpOpen(
        detail::utf8_to_wide(user_agent).c_str(),
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        out.error = "WinHttpOpen failed";
        return out;
    }
    scoped_exit close_session{[&] { WinHttpCloseHandle(session); }};

    // 30s connect / send / receive
    WinHttpSetTimeouts(session, 30000, 30000, 30000, 30000);

    HINTERNET conn = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    if (!conn) {
        out.error = "WinHttpConnect failed";
        return out;
    }
    scoped_exit close_conn{[&] { WinHttpCloseHandle(conn); }};

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(
        conn, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        out.error = "WinHttpOpenRequest failed";
        return out;
    }
    scoped_exit close_req{[&] { WinHttpCloseHandle(req); }};

    // Follow redirects across HTTP/HTTPS.
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        out.error = "WinHttpSendRequest failed";
        return out;
    }
    if (!WinHttpReceiveResponse(req, nullptr)) {
        out.error = "WinHttpReceiveResponse failed";
        return out;
    }

    DWORD status = 0;
    DWORD status_len = sizeof(status);
    WinHttpQueryHeaders(req,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                        WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) {
            out.error = "WinHttpQueryDataAvailable failed";
            return out;
        }
        if (avail == 0) break;
        // Cap body at 16 MiB — subscription YAMLs are far smaller.
        if (out.body.size() + avail > 16u * 1024u * 1024u) {
            out.error = "response body exceeds 16 MiB";
            return out;
        }
        const size_t off = out.body.size();
        out.body.resize(off + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, out.body.data() + off, avail, &read)) {
            out.error = "WinHttpReadData failed";
            return out;
        }
        out.body.resize(off + read);
    }

    if (out.status < 200 || out.status >= 300) {
        out.error = "HTTP status " + std::to_string(out.status);
    }
    return out;
}

} // namespace clew
