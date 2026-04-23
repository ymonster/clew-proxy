#pragma once

// WinDivert SOCKET layer: SNIFF + RECV_ONLY mode.
// Observes outbound TCP connect() events, writes matching connections
// to PortTracker for NETWORK layer to NAT.
//
// Integrates with Asio IOCP via overlapped_ptr (verified in Phase 3 PoC).
// Handler runs on strand — direct access to flat_tree side_map, no locks.
//
// WinDivert constraint: SOCKET layer requires WINDIVERT_FLAG_RECV_ONLY.
// No WinDivertSend needed — SNIFF mode auto-passes events.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <windivert.h>

#define ASIO_STANDALONE
#include <asio.hpp>

#include <cstdio>
#include <cstring>
#include <format>
#include <atomic>
#include <unordered_map>
#include <vector>
#include "core/log.hpp"

#include "process/flat_tree.hpp"
#include "core/port_tracker.hpp"

namespace clew {

class windivert_socket {
public:
    windivert_socket(asio::io_context& ioc,
                     asio::strand<asio::io_context::executor_type>& strand,
                     flat_tree& tree,
                     PortTracker& tracker)
        : ioc_(ioc)
        , strand_(strand)
        , tree_(tree)
        , tracker_(tracker)
    {}

    ~windivert_socket() { close(); }

    bool open() {
        DWORD self_pid = GetCurrentProcessId();

        auto filter = std::format(
            "outbound and !loopback and tcp "
            "and event == CONNECT "
            "and processId != {}",
            self_pid);

        handle_ = WinDivertOpen(filter.c_str(), WINDIVERT_LAYER_SOCKET, 0,
                                WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);

        if (handle_ == INVALID_HANDLE_VALUE) {
            PC_LOG_ERROR("[WD-SOCKET] Open failed: {}", GetLastError());
            return false;
        }

        // Register with Asio IOCP
        std::error_code ec;
        asio::use_service<asio::detail::win_iocp_io_context>(ioc_)
            .register_handle(handle_, ec);

        if (ec) {
            PC_LOG_WARN("[WD-SOCKET] IOCP register failed: {}, using blocking fallback",
                         ec.message());
            use_iocp_ = false;
        } else {
            use_iocp_ = true;
        }

        PC_LOG_INFO("[WD-SOCKET] Opened (SNIFF+RECV_ONLY, IOCP={})", use_iocp_);
        return true;
    }

    void start() {
        if (handle_ == INVALID_HANDLE_VALUE) return;
        running_ = true;

        if (use_iocp_) {
            async_recv();
        } else {
            // Fallback: blocking thread posts to strand
            blocking_thread_ = std::jthread([this]() { blocking_recv_loop(); });
        }
    }

    void close() {
        running_ = false;
        if (handle_ != INVALID_HANDLE_VALUE) {
            WinDivertClose(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        if (blocking_thread_.joinable()) blocking_thread_.join();
        PC_LOG_INFO("[WD-SOCKET] Closed");
    }

    uint64_t event_count() const { return event_count_; }
    uint64_t match_count() const { return match_count_; }

private:
    asio::io_context& ioc_;
    asio::strand<asio::io_context::executor_type>& strand_;
    flat_tree& tree_;
    PortTracker& tracker_;

    HANDLE handle_{INVALID_HANDLE_VALUE};
    bool use_iocp_{true};
    std::atomic<bool> running_{false};
    std::jthread blocking_thread_;

    WINDIVERT_ADDRESS addr_{};
    UINT addr_len_{sizeof(WINDIVERT_ADDRESS)};

    uint64_t event_count_{0};
    uint64_t match_count_{0};

    // ---- Asio IOCP async path ----

    void async_recv() {
        if (!running_) return;

        asio::windows::overlapped_ptr op{
            strand_,
            [this](std::error_code ec, std::size_t) {
                if (!ec && running_) {
                    on_socket_event(addr_);
                    async_recv();
                }
            }
        };

        BOOL ok = WinDivertRecvEx(
            handle_, nullptr, 0, nullptr, 0,
            &addr_, &addr_len_, op.get());

        DWORD err = GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            op.release();
        } else if (ok) {
            op.complete(std::error_code{}, 0);
        } else {
            if (running_)
                PC_LOG_ERROR("[WD-SOCKET] RecvEx error: {}", err);
        }
    }

    // ---- Blocking fallback path ----

    void blocking_recv_loop() {
        WINDIVERT_ADDRESS addr;
        while (running_) {
            UINT recv_len = 0;
            if (!WinDivertRecv(handle_, nullptr, 0, &recv_len, &addr)) {
                DWORD err = GetLastError();
                if (err == ERROR_NO_DATA || err == ERROR_INVALID_HANDLE) break;
                continue;
            }
            asio::post(strand_, [this, addr]() {
                on_socket_event(addr);
            });
        }
    }

    // ---- Event handler (runs on strand) ----

    void on_socket_event(const WINDIVERT_ADDRESS& addr) {
        if (addr.Event != WINDIVERT_EVENT_SOCKET_CONNECT) return;
        // SNIFF mode: no WinDivertSend needed, event auto-passes

        event_count_++;

        const DWORD pid = addr.Socket.ProcessId;
        const uint16_t src_port = static_cast<uint16_t>(addr.Socket.LocalPort);

        // Lookup in flat tree (strand-safe, no locks)
        uint32_t idx = tree_.find_by_pid(pid);
        if (idx == INVALID_IDX) return;

        const auto& entry = tree_.at(idx);
        if (!entry.alive || !entry.is_proxied()) return;

        // Write to PortTracker (release semantics for NETWORK workers)
        TrackerEntry te{};
        std::memcpy(te.remote_addr, addr.Socket.RemoteAddr, sizeof(te.remote_addr));
        te.remote_port = static_cast<uint16_t>(addr.Socket.RemotePort);
        te.group_id = entry.group_id;

        tracker_.put(src_port, te);
        match_count_++;

        PC_LOG_DEBUG("[WD-SOCKET] Match PID={} port={} -> {}:{} group={}",
                      pid, src_port,
                      (te.remote_addr[0] >> 0) & 0xFF,  // just first octet for debug
                      te.remote_port, te.group_id);
    }
};

} // namespace clew
