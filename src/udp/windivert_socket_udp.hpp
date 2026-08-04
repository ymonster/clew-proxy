#pragma once

// WinDivert SOCKET layer for UDP: SNIFF + RECV_ONLY mode.
// Observes UDP bind() and connect() events, writes matching entries
// to UdpPortTracker for UDP NETWORK layer to intercept.
//
// Independent from TCP SOCKET layer — separate handle, filter, tracker.
// Same IOCP/blocking fallback pattern as TCP version.

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
#include "core/log.hpp"

#include "process/flat_tree.hpp"
#include "rules/rule_engine_v3.hpp"
#include "udp/udp_port_tracker.hpp"

namespace clew {

class windivert_socket_udp {
public:
    windivert_socket_udp(asio::io_context& ioc,
                         asio::strand<asio::io_context::executor_type>& strand,
                         flat_tree& tree,
                         rule_engine_v3& rules,
                         UdpPortTracker& tracker)
        : ioc_(ioc)
        , strand_(strand)
        , tree_(tree)
        , rules_(rules)
        , tracker_(tracker)
    {}

    ~windivert_socket_udp() { close(); }

    bool open() {
        DWORD self_pid = GetCurrentProcessId();

        auto filter = std::format(
            "udp and !loopback and processId != {}",
            self_pid);

        handle_ = WinDivertOpen(filter.c_str(), WINDIVERT_LAYER_SOCKET, 0,
                                WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);

        if (handle_ == INVALID_HANDLE_VALUE) {
            PC_LOG_ERROR("[WD-SOCKET-UDP] Open failed: {}", GetLastError());
            return false;
        }

        // Register with Asio IOCP
        std::error_code ec;
        asio::use_service<asio::detail::win_iocp_io_context>(ioc_)
            .register_handle(handle_, ec);

        if (ec) {
            PC_LOG_WARN("[WD-SOCKET-UDP] IOCP register failed: {}, using blocking fallback",
                         ec.message());
            use_iocp_ = false;
        } else {
            use_iocp_ = true;
        }

        PC_LOG_INFO("[WD-SOCKET-UDP] Opened (SNIFF+RECV_ONLY, IOCP={})", use_iocp_);
        return true;
    }

    void start() {
        if (handle_ == INVALID_HANDLE_VALUE) return;
        running_ = true;

        if (use_iocp_) {
            async_recv();
        } else {
            blocking_thread_ = std::jthread([this]() { blocking_recv_loop(); });
        }
    }

    void close() {
        if (closed_) return;
        closed_ = true;
        running_ = false;
        if (handle_ != INVALID_HANDLE_VALUE) {
            WinDivertClose(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        if (blocking_thread_.joinable()) blocking_thread_.join();
        PC_LOG_INFO("[WD-SOCKET-UDP] Closed (events={}, matches={})", event_count_, match_count_);
    }

    uint64_t event_count() const { return event_count_; }
    uint64_t match_count() const { return match_count_; }

private:
    asio::io_context& ioc_;
    asio::strand<asio::io_context::executor_type>& strand_;
    flat_tree& tree_;
    rule_engine_v3& rules_;
    UdpPortTracker& tracker_;

    HANDLE handle_{INVALID_HANDLE_VALUE};
    bool use_iocp_{true};
    bool closed_{false};
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
                PC_LOG_ERROR("[WD-SOCKET-UDP] RecvEx error: {}", err);
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
        // Accept both BIND and CONNECT for UDP
        if (addr.Event != WINDIVERT_EVENT_SOCKET_BIND &&
            addr.Event != WINDIVERT_EVENT_SOCKET_CONNECT) {
            // Handle CLOSE: clear tracker entry
            if (addr.Event == WINDIVERT_EVENT_SOCKET_CLOSE) {
                uint16_t port = static_cast<uint16_t>(addr.Socket.LocalPort);
                tracker_.clear(port);
            }
            return;
        }

        event_count_++;

        const DWORD pid = addr.Socket.ProcessId;
        const uint16_t src_port = static_cast<uint16_t>(addr.Socket.LocalPort);
        const bool is_connect = (addr.Event == WINDIVERT_EVENT_SOCKET_CONNECT);

        // Check if this PID should be proxied for UDP protocol
        if (!rules_.should_proxy_protocol(tree_, pid, "udp")) return;

        // Lookup group_id from flat_tree
        uint32_t idx = tree_.find_by_pid(pid);
        if (idx == INVALID_IDX) return;
        const auto& entry = tree_.at(idx);

        if (is_connect) {
            // A connected socket has one fixed destination, so the exclude
            // decision can be made once here instead of on every packet. When
            // it is excluded the slot goes away and the traffic never enters
            // the reflect/relay path at all.
            const uint32_t dest_ip = addr.Socket.RemoteAddr[0];
            const auto reason = rules_.ip_exclude_reason(tree_, pid, dest_ip);
            if (reason != IpExcludeReason::none) {
                tracker_.clear(src_port);
                PC_LOG_DEBUG("[WD-SOCKET-UDP] Direct PID={} port={} -> {}:{} reason={}",
                              pid, src_port, CidrRange::uint_to_ip(dest_ip),
                              static_cast<uint16_t>(addr.Socket.RemotePort),
                              ip_exclude_reason_name(reason));
                return;
            }
            // BIND already published this slot and CONNECT carries nothing new
            // to store. Writing it again would overwrite an entry the NETWORK
            // workers are likely already reading.
            if (tracker_.is_active(src_port)) return;
        }

        // Write to UdpPortTracker (release semantics for NETWORK workers)
        UdpTrackerEntry te{};
        te.group_id = entry.group_id;
        te.pid = pid;
        te.policy_id = rules_.policy_id_for(tree_, pid);

        tracker_.put(src_port, te);
        match_count_++;

        PC_LOG_DEBUG("[WD-SOCKET-UDP] Match PID={} port={} event={} group={} policy={}",
                      pid, src_port, is_connect ? "CONNECT" : "BIND",
                      te.group_id, te.policy_id);
    }
};

} // namespace clew
