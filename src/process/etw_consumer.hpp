#pragma once

// ETW real-time consumer for process start/stop events.
// Uses Microsoft-Windows-Kernel-Process provider.
// Events are delivered via a callback; the caller posts them to a strand.
//
// PoC lessons (Phase 2a):
//   - Win10 19045: ProcessStart=V3, ProcessStop=V2 (higher than docs)
//   - V2+ adds 8-byte ProcessSequenceNumber after PID, shifting offsets +8
//   - ProcessStart ImageName: wchar_t* (Unicode) at offset 48 (V3) or 36 (V2)
//   - ProcessStop ImageName: char* (ANSI!) at offset 84
//   - EventHeader.ProcessId ≠ target PID, must parse from UserData
//   - ProcessTrace() blocks, must run on dedicated thread
//   - Orphaned sessions must be cleaned up before StartTrace

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <atomic>
#include <thread>
#include <functional>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <vector>
#include "core/log.hpp"

#pragma comment(lib, "advapi32.lib")

namespace clew {

// Event data extracted from ETW callback
struct etw_process_event {
    enum class type_t { START, STOP };
    using enum type_t;  // allow etw_process_event::START / ::STOP at class scope
    type_t   type;
    DWORD    pid;
    DWORD    parent_pid;    // only valid for START
    FILETIME create_time;
    wchar_t  image_name[260];  // wide string (converted from ANSI for STOP)
    std::chrono::steady_clock::time_point received_at;  // for latency measurement
};

class etw_consumer {
public:
    using event_callback = std::function<void(const etw_process_event&)>;

    explicit etw_consumer(event_callback cb) : callback_(std::move(cb)) {}
    ~etw_consumer() { stop(); }

    bool start() {
        if (running_) return true;

        // Clean up orphaned session
        cleanup_session();

        // Start trace session. EVENT_TRACE_PROPERTIES is a variable-size struct
        // (fixed header + trailing LoggerName buffer); use a byte vector as
        // RAII-managed backing storage. value-initialized to zero (≡ calloc).
        const size_t props_size = sizeof(EVENT_TRACE_PROPERTIES) +
                                  (wcslen(SESSION_NAME) + 1) * sizeof(wchar_t);
        std::vector<std::byte> props_buf(props_size);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props_buf.data());

        props->Wnode.BufferSize = static_cast<ULONG>(props_size);
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;  // QPC clock
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        props->BufferSize = 64;
        props->MinimumBuffers = 4;
        props->MaximumBuffers = 16;
        props->FlushTimer = 1;

        ULONG status = StartTraceW(&session_handle_, SESSION_NAME, props);

        if (status != ERROR_SUCCESS) {
            PC_LOG_ERROR("[ETW] StartTrace failed: {}", status);
            return false;
        }

        // Enable provider
        status = EnableTraceEx2(
            session_handle_, &KERNEL_PROCESS_GUID,
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            TRACE_LEVEL_INFORMATION,
            KEYWORD_PROCESS, 0, 0, nullptr);

        if (status != ERROR_SUCCESS) {
            PC_LOG_ERROR("[ETW] EnableTraceEx2 failed: {}", status);
            stop_session();
            return false;
        }

        // Open trace for consumption
        EVENT_TRACE_LOGFILEW logfile{};
        logfile.LoggerName = SESSION_NAME;
        logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD |
                                   PROCESS_TRACE_MODE_REAL_TIME;
        logfile.Context = this;
        logfile.EventRecordCallback = event_record_callback;
        logfile.BufferCallback = buffer_callback;

        trace_handle_ = OpenTraceW(&logfile);
        if (trace_handle_ == INVALID_PROCESSTRACE_HANDLE) {
            PC_LOG_ERROR("[ETW] OpenTrace failed: {}", GetLastError());
            stop_session();
            return false;
        }

        // ProcessTrace blocks — run on dedicated thread
        running_ = true;
        trace_thread_ = std::jthread([this]() {
            ULONG rc = ProcessTrace(&trace_handle_, 1, nullptr, nullptr);
            if (rc != ERROR_SUCCESS && rc != ERROR_CANCELLED && running_)
                PC_LOG_WARN("[ETW] ProcessTrace returned: {}", rc);
        });

        PC_LOG_INFO("[ETW] Consumer started");
        return true;
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        stop_session();
        if (trace_handle_ != INVALID_PROCESSTRACE_HANDLE) {
            CloseTrace(trace_handle_);
            trace_handle_ = INVALID_PROCESSTRACE_HANDLE;
        }
        if (trace_thread_.joinable()) trace_thread_.join();
        PC_LOG_INFO("[ETW] Consumer stopped");
    }

    bool is_running() const { return running_; }

private:
    static constexpr GUID KERNEL_PROCESS_GUID = {
        0x22fb2cd6, 0x0e7b, 0x422b,
        { 0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16 }
    };
    static constexpr ULONGLONG KEYWORD_PROCESS = 0x10;
    static constexpr USHORT EVENT_PROCESS_START = 1;
    static constexpr USHORT EVENT_PROCESS_STOP  = 2;
    // Non-const: EVENT_TRACE_LOGFILEW::LoggerName is typedef'd LPWSTR (non-const)
    // even though the consumer path only reads it. Avoids const_cast at the call site.
    static inline wchar_t SESSION_NAME[] = L"ClewETW";

    event_callback callback_;
    std::atomic<bool> running_{false};
    TRACEHANDLE session_handle_{0};
    TRACEHANDLE trace_handle_{INVALID_PROCESSTRACE_HANDLE};
    std::jthread trace_thread_;

    // Decode a ProcessStart record's UserData into evt.
    // Returns false if the buffer is too short to be valid.
    static bool decode_start_event(const BYTE* data, USHORT len, etw_process_event& evt) {
        if (len < 36) return false;

        evt.type = etw_process_event::START;
        evt.pid = *reinterpret_cast<const UINT32*>(data + 0);
        std::memcpy(&evt.create_time, data + 12, sizeof(FILETIME));
        evt.parent_pid = *reinterpret_cast<const UINT32*>(data + 20);

        // ImageName: scan for path prefix instead of fixed offset.
        // V3 has 11 fields (ImageName=%11), V2 has 6 (ImageName=%6).
        // V3 adds ProcessSequenceNumber, ParentProcessSequenceNumber,
        // ImageChecksum, TimeDateStamp, PackageFullName between
        // SessionID and ImageName. Fixed offset unreliable across versions.
        evt.image_name[0] = L'\0';
        for (int off = 36; off < len - 4; off += 2) {
            wchar_t c = *reinterpret_cast<const wchar_t*>(data + off);
            if (c != L'\\') continue;

            const wchar_t* img   = reinterpret_cast<const wchar_t*>(data + off);
            const wchar_t* slash = wcsrchr(img, L'\\');
            const wchar_t* name  = slash ? slash + 1 : img;
            wcsncpy_s(evt.image_name, std::size(evt.image_name), name, _TRUNCATE);
            break;
        }
        return true;
    }

    // Decode a ProcessStop record's UserData into evt.
    // Returns false if the buffer is too short to be valid.
    static bool decode_stop_event(const BYTE* data, USHORT len, etw_process_event& evt) {
        if (len < 85) return false;

        evt.type = etw_process_event::STOP;
        evt.pid = *reinterpret_cast<const UINT32*>(data + 0);
        std::memcpy(&evt.create_time, data + 12, sizeof(FILETIME));
        evt.parent_pid = 0;

        // ImageName at offset 84 (ANSI — asymmetry with ProcessStart's UTF-16).
        // Verify NUL-terminated within bounds before strrchr touches it.
        const char* ansi_name = reinterpret_cast<const char*>(data + 84);
        bool valid = false;
        for (int i = 84; i < len; i++) {
            if (data[i] == 0) { valid = true; break; }
        }
        if (!valid) return true;  // event delivered, name stays empty

        const char* slash = strrchr(ansi_name, '\\');
        const char* name  = slash ? slash + 1 : ansi_name;
        MultiByteToWideChar(CP_ACP, 0, name, -1, evt.image_name, 260);
        return true;
    }

    static void WINAPI event_record_callback(PEVENT_RECORD pEvent) {
        const auto* self = static_cast<const etw_consumer*>(pEvent->UserContext);
        if (!self || !self->running_) return;
        if (!IsEqualGUID(pEvent->EventHeader.ProviderId, KERNEL_PROCESS_GUID)) return;

        const USHORT id  = pEvent->EventHeader.EventDescriptor.Id;
        const BYTE* data = static_cast<const BYTE*>(pEvent->UserData);
        const USHORT len = pEvent->UserDataLength;

        etw_process_event evt{};
        evt.received_at = std::chrono::steady_clock::now();

        bool decoded = false;
        if (id == EVENT_PROCESS_START) {
            decoded = decode_start_event(data, len, evt);
        } else if (id == EVENT_PROCESS_STOP) {
            decoded = decode_stop_event(data, len, evt);
        }
        if (!decoded) return;

        self->callback_(evt);
    }

    static ULONG WINAPI buffer_callback(PEVENT_TRACE_LOGFILEW logfile) {
        const auto* self = static_cast<const etw_consumer*>(logfile->Context);
        return (self && self->running_) ? TRUE : FALSE;
    }

    void cleanup_session() noexcept {
        try {
            const size_t buf_size = sizeof(EVENT_TRACE_PROPERTIES) +
                                    (wcslen(SESSION_NAME) + 1) * sizeof(wchar_t);
            std::vector<std::byte> props_buf(buf_size);
            auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props_buf.data());
            props->Wnode.BufferSize = static_cast<ULONG>(buf_size);
            props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            ControlTraceW(0, SESSION_NAME, props, EVENT_TRACE_CONTROL_STOP);
        } catch (const std::bad_alloc&) {
            PC_LOG_WARN("[etw] cleanup_session: OOM allocating props buffer; skipping orphan reap");
        }
    }

    void stop_session() noexcept {
        if (session_handle_ == 0) return;
        EnableTraceEx2(session_handle_, &KERNEL_PROCESS_GUID,
                       EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, 0, nullptr);
        try {
            const size_t buf_size = sizeof(EVENT_TRACE_PROPERTIES) +
                                    (wcslen(SESSION_NAME) + 1) * sizeof(wchar_t);
            std::vector<std::byte> props_buf(buf_size);
            auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props_buf.data());
            props->Wnode.BufferSize = static_cast<ULONG>(buf_size);
            props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            ControlTraceW(session_handle_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
        } catch (const std::bad_alloc&) {
            PC_LOG_WARN("[etw] stop_session: OOM allocating props buffer; "
                        "kernel session will be reaped on next start");
        }
        session_handle_ = 0;
    }
};

} // namespace clew
