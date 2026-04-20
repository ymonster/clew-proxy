#pragma once

// WinDivert NETWORK layer: Reflection mode with batch I/O.
// Redirects matched TCP traffic to local listener using addr swap + inbound reinject.
// Pattern matches official WinDivert streamdump.exe example.
//
// Design (from architecture_redesign_v3.md §3.5):
//   - 2-4 dedicated blocking worker threads (NOT Asio IOCP)
//   - Batch RecvEx/SendEx for throughput
//   - Hot path: O(1) PortTracker lookup → passthrough if no match
//   - Cold path: NAT rewrite + checksum recalc
//   - All non-loopback outbound TCP is captured; filter: "outbound and tcp and !loopback"
//
// Reflection (方案 B):
//   Forward: swap(SrcAddr, DstAddr), DstPort=redirect_port, Outbound=0 → inbound reinject
//   Reverse: proxy reply is also outbound non-loopback, swap back, Outbound=0 → inbound reinject

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

#include "core/port_tracker.hpp"

namespace clew {

class windivert_network {
public:
    windivert_network(PortTracker& tracker, uint16_t redirect_port)
        : tracker_(tracker)
        , redirect_port_(redirect_port)
    {}

    ~windivert_network() { close(); }

    bool open() {
        const char* filter = "outbound and tcp and !loopback";

        handle_ = WinDivertOpen(filter, WINDIVERT_LAYER_NETWORK, 0, 0);
        if (handle_ == INVALID_HANDLE_VALUE) {
            PC_LOG_ERROR("[WD-NETWORK] Open failed: {}", GetLastError());
            return false;
        }

        // Tune queue params for high throughput
        WinDivertSetParam(handle_, WINDIVERT_PARAM_QUEUE_LENGTH, 16384);
        WinDivertSetParam(handle_, WINDIVERT_PARAM_QUEUE_TIME, 2000);
        WinDivertSetParam(handle_, WINDIVERT_PARAM_QUEUE_SIZE, 16 * 1024 * 1024);

        PC_LOG_INFO("[WD-NETWORK] Opened (Reflection, filter: {})", filter);
        return true;
    }

    void start(int num_workers = 2) {
        running_ = true;
        for (int i = 0; i < num_workers; i++) {
            workers_.emplace_back([this](std::stop_token st) {
                worker_loop(st);
            });
        }
        PC_LOG_INFO("[WD-NETWORK] {} worker threads started", num_workers);
    }

    void close() {
        running_ = false;
        for (auto& w : workers_) w.request_stop();
        if (handle_ != INVALID_HANDLE_VALUE) {
            WinDivertClose(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        workers_.clear();
        PC_LOG_INFO("[WD-NETWORK] Closed");
    }

    uint64_t nat_count() const { return nat_count_; }
    uint64_t pass_count() const { return pass_count_; }

private:
    PortTracker& tracker_;
    uint16_t redirect_port_;
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::atomic<bool> running_{false};
    std::vector<std::jthread> workers_;

    std::atomic<uint64_t> nat_count_{0};
    std::atomic<uint64_t> pass_count_{0};

    void worker_loop(std::stop_token st) {
        // Stack-allocated buffers per worker, zero heap allocation
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
            PWINDIVERT_TCPHDR tcp = nullptr;

            WinDivertHelperParsePacket(
                pkt_buf, pkt_len,
                &ip, nullptr, nullptr, nullptr, nullptr,
                &tcp, nullptr, nullptr, nullptr, nullptr, nullptr);

            if (!ip || !tcp) {
                // Non-TCP/IP packet — passthrough unchanged
                WinDivertSend(handle_, pkt_buf, pkt_len, nullptr, &addr);
                continue;
            }

            uint16_t src_port = ntohs(tcp->SrcPort);
            uint16_t dst_port = ntohs(tcp->DstPort);

            // === Hot path: check if this connection is tracked ===
            // Outbound from tracked app: src_port is the app's ephemeral port
            if (tracker_.is_active(src_port)) {
                // Cold path: NAT rewrite (Reflection)
                reflect_outbound(pkt_buf, pkt_len, ip, tcp, src_port, &addr);
                nat_count_.fetch_add(1, std::memory_order_relaxed);
            }
            // Outbound proxy reply: the relay sent data to the original destination IP
            // In Reflection mode, this appears as outbound to the original dest.
            // We need to check if dst_port matches any tracked connection's remote_port.
            // Actually, the reply from the listener goes to the original dest IP:port,
            // so it shows as outbound. The src_port is the listener's ephemeral port
            // to the upstream proxy. But wait — the relay connects to the SOCKS5 proxy,
            // not to the original destination. So the reply path is different.
            //
            // In Reflection: after the initial SYN is reflected inbound, the TCP handshake
            // completes with the listener. Subsequent data from the app is ALSO outbound
            // to the original dest (kernel still thinks it's connected to orig dest).
            // All these packets have src_port = app's port, which IS in the tracker.
            //
            // Reply from listener → app: the listener sends to orig_dest_ip:orig_dest_port
            // as the TCP peer. In Reflection mode, this is also outbound non-loopback.
            // The src_port is the redirect_port.
            else if (src_port == redirect_port_) {
                // Reply from local listener — reflect back to app
                reflect_reply(pkt_buf, pkt_len, ip, tcp, dst_port, &addr);
                nat_count_.fetch_add(1, std::memory_order_relaxed);
            }
            else {
                // Passthrough: not a tracked connection
                WinDivertSend(handle_, pkt_buf, pkt_len, nullptr, &addr);
                pass_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Reflection forward: app → original_dest becomes inbound to listener
    void reflect_outbound(uint8_t* pkt_buf, UINT pkt_len,
                          PWINDIVERT_IPHDR ip, PWINDIVERT_TCPHDR tcp,
                          uint16_t src_port, WINDIVERT_ADDRESS* addr)
    {
        // Swap src/dst addresses
        uint32_t tmp = ip->SrcAddr;
        ip->SrcAddr = ip->DstAddr;
        ip->DstAddr = tmp;

        // Set DstPort to redirect port (listener)
        tcp->DstPort = htons(redirect_port_);

        // Reinject as inbound (Reflection pattern)
        addr->Outbound = 0;

        WinDivertHelperCalcChecksums(pkt_buf, pkt_len, addr, 0);

        if (!WinDivertSend(handle_, pkt_buf, pkt_len, nullptr, addr)) {
            PC_LOG_ERROR("[WD-NETWORK] Send (reflect out) failed: {}", GetLastError());
        }
    }

    // Reflection reply: listener → app (appears as outbound to orig dest)
    // Before: SrcAddr=AppIP, SrcPort=redirect_port, DstAddr=OrigDstIP, DstPort=AppPort
    // After:  SrcAddr=OrigDstIP, SrcPort=OrigDstPort, DstAddr=AppIP, DstPort=AppPort, Outbound=0
    void reflect_reply(uint8_t* pkt_buf, UINT pkt_len,
                       PWINDIVERT_IPHDR ip, PWINDIVERT_TCPHDR tcp,
                       uint16_t app_port, WINDIVERT_ADDRESS* addr)
    {
        // Lookup original destination from tracker (entry persists until connection close)
        if (!tracker_.is_active(app_port)) {
            // Unknown or already-closed connection — passthrough
            WinDivertSend(handle_, pkt_buf, pkt_len, nullptr, addr);
            return;
        }

        const auto& entry = tracker_.peek(app_port);

        // Swap src/dst addresses
        uint32_t tmp = ip->SrcAddr;
        ip->SrcAddr = ip->DstAddr;
        ip->DstAddr = tmp;

        // Restore SrcPort to original destination port
        // (app expects replies from OrigDstIP:OrigDstPort)
        tcp->SrcPort = htons(entry.remote_port);

        // Reinject as inbound
        addr->Outbound = 0;

        WinDivertHelperCalcChecksums(pkt_buf, pkt_len, addr, 0);

        if (!WinDivertSend(handle_, pkt_buf, pkt_len, nullptr, addr)) {
            PC_LOG_ERROR("[WD-NETWORK] Send (reflect reply) failed: {}", GetLastError());
        }
    }
};

} // namespace clew
