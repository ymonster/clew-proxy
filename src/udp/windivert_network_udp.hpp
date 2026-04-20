#pragma once

// WinDivert NETWORK layer for UDP: intercept + reflect + inject.
// Independent from TCP NETWORK layer — separate handle, filter, workers.
//
// Forward path (app -> relay):
//   Outbound UDP from tracked port → swap addrs, DstPort=relay_port, Outbound=0
//   Session info saved to UdpSessionTable for relay to restore original dst.
//
// Reply injection is done by the relay via the shared handle (WinDivertSend).
// This class exposes handle() for that purpose.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windivert.h>

#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include "core/log.hpp"

#include "udp/udp_port_tracker.hpp"
#include "udp/udp_session_table.hpp"

namespace clew {

class windivert_network_udp {
public:
    windivert_network_udp(UdpPortTracker& tracker, uint16_t relay_port,
                          UdpSessionTable& session_table)
        : tracker_(tracker)
        , relay_port_(relay_port)
        , session_table_(session_table)
    {}

    ~windivert_network_udp() { close(); }

    bool open() {
        const char* filter = "udp and outbound and !loopback";

        handle_ = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 0, 0);
        if (handle_ == INVALID_HANDLE_VALUE) {
            PC_LOG_ERROR("[WD-NET-UDP] Open failed: {}", GetLastError());
            return false;
        }

        WinDivertSetParam(handle_, WINDIVERT_PARAM_QUEUE_LENGTH, 16384);
        WinDivertSetParam(handle_, WINDIVERT_PARAM_QUEUE_TIME, 2000);
        WinDivertSetParam(handle_, WINDIVERT_PARAM_QUEUE_SIZE, 16 * 1024 * 1024);

        PC_LOG_INFO("[WD-NET-UDP] Opened (filter: {})", filter);
        return true;
    }

    void start(int num_workers = 2) {
        running_ = true;
        for (int i = 0; i < num_workers; i++) {
            workers_.emplace_back([this](std::stop_token st) {
                worker_loop(st);
            });
        }
        PC_LOG_INFO("[WD-NET-UDP] {} workers started", num_workers);
    }

    void close() {
        if (closed_) return;
        closed_ = true;
        running_ = false;
        for (auto& w : workers_) w.request_stop();
        if (handle_ != INVALID_HANDLE_VALUE) {
            WinDivertClose(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        workers_.clear();
        PC_LOG_INFO("[WD-NET-UDP] Closed (reflected={}, passed={})",
                     reflect_count_.load(), pass_count_.load());
    }

    // Expose handle for reply injection from relay
    HANDLE handle() const { return handle_; }

    uint64_t reflect_count() const { return reflect_count_; }
    uint64_t pass_count() const { return pass_count_; }

private:
    UdpPortTracker& tracker_;
    uint16_t relay_port_;
    UdpSessionTable& session_table_;

    HANDLE handle_{INVALID_HANDLE_VALUE};
    bool closed_{false};
    std::atomic<bool> running_{false};
    std::vector<std::jthread> workers_;

    std::atomic<uint64_t> reflect_count_{0};
    std::atomic<uint64_t> pass_count_{0};

    void worker_loop(std::stop_token st) {
        uint8_t pkt_buf[65535];
        WINDIVERT_ADDRESS addr;

        while (!st.stop_requested() && running_) {
            UINT pkt_len = sizeof(pkt_buf);
            if (!WinDivertRecv(handle_, pkt_buf, pkt_len, &pkt_len, &addr)) {
                DWORD err = GetLastError();
                if (err == ERROR_NO_DATA || err == ERROR_INVALID_HANDLE) break;
                continue;
            }

            PWINDIVERT_IPHDR ip = nullptr;
            PWINDIVERT_UDPHDR udp = nullptr;
            PVOID payload = nullptr;
            UINT payload_len = 0;

            WinDivertHelperParsePacket(
                pkt_buf, pkt_len,
                &ip, nullptr, nullptr, nullptr, nullptr,
                nullptr, &udp, &payload, &payload_len, nullptr, nullptr);

            if (!ip || !udp) {
                WinDivertSend(handle_, pkt_buf, pkt_len, nullptr, &addr);
                continue;
            }

            uint16_t src_port = ntohs(udp->SrcPort);

            // Check if this port is tracked for UDP proxy
            if (!tracker_.is_active(src_port)) {
                // Not tracked — passthrough
                WinDivertSend(handle_, pkt_buf, pkt_len, nullptr, &addr);
                pass_count_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            const auto& te = tracker_.peek(src_port);

            // Save session info BEFORE modifying packet
            UdpSession session{};
            session.orig_dst_addr = ip->DstAddr;
            session.orig_dst_port = udp->DstPort;
            session.app_src_addr  = ip->SrcAddr;
            session.app_src_port  = src_port;
            session.pid           = te.pid;
            session.group_id      = te.group_id;
            session.ifidx         = addr.Network.IfIdx;
            session.subifidx      = addr.Network.SubIfIdx;
            session_table_.upsert(src_port, session);

            // Reflection: swap addrs, set DstPort=relay, flip to inbound
            uint32_t tmp = ip->SrcAddr;
            ip->SrcAddr = ip->DstAddr;
            ip->DstAddr = tmp;

            udp->DstPort = htons(relay_port_);
            // SrcPort unchanged — relay uses it to identify the app

            addr.Outbound = 0;
            WinDivertHelperCalcChecksums(pkt_buf, pkt_len, &addr, 0);

            if (!WinDivertSend(handle_, pkt_buf, pkt_len, nullptr, &addr)) {
                PC_LOG_WARN("[WD-NET-UDP] Reflect send failed: {}", GetLastError());
            } else {
                reflect_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
};

} // namespace clew
