#pragma once

// RAII guard for Winsock initialization. Ctor calls WSAStartup; dtor calls
// WSACleanup. Movable-only by deletion. Construct once near the top of main()
// and let RAII unwind on shutdown.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>

namespace clew {

class winsock_session {
public:
    winsock_session() noexcept {
        WSADATA wsaData;
        ok_ = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
    }

    ~winsock_session() noexcept {
        if (ok_) WSACleanup();
    }

    winsock_session(const winsock_session&)            = delete;
    winsock_session& operator=(const winsock_session&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

} // namespace clew
