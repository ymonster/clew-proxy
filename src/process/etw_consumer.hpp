#pragma once

// ETW consumer for Microsoft-Windows-Kernel-Process.
//
// Architecture (post-PoC, 2026-05-09):
//   - Provider: Microsoft-Windows-Kernel-Process (manifest-based)
//   - Internal-side filter: EnableTraceEx2 + EVENT_FILTER_TYPE_EVENT_ID
//     restricts to {1=ProcessStart, 2=ProcessStop, 15=ProcessRundown}
//   - Decode: TDH-once schema cache + per-event sliding pointer (no
//     fixed-offset hardcoding — that was the candidate-F root cause of
//     the sticky tree bug we just shipped a fix for).
//   - Rundown: EVENT_CONTROL_CODE_CAPTURE_STATE on the same session/provider
//     emits ProcessRundown(15) with the same schema as ProcessStart v3.
//     This replaces NtQuerySystemInformation startup snapshots entirely.
//   - Lost detection: BufferCallback observes EVENT_TRACE_LOGFILEW.EventsLost
//     (cumulative) and posts an EVENTS_LOST DTO each time it grows. The
//     manager debounces and re-issues capture_state.
//
// PoC verification report:
//   F:/projects/remote_projects/win_prox/tools/poc/poc_etw_kernel_process_v2_report.md
// Schema reference (field names, InTypes, sentinel values):
//   ~/.claude/projects/F--projects-remote-projects-win-prox/memory/reference/etw_kernel_process.md

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <evntcons.h>
#include <evntprov.h>
#include <evntrace.h>
#include <tdh.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/log.hpp"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "tdh.lib")

namespace clew {

// Event kinds carried by the DTO. EVENTS_LOST is synthetic — emitted by
// the buffer callback when ETW reports drops; the manager decides whether
// to re-issue capture_state.
enum class etw_process_event_kind : uint8_t {
    START,
    STOP,
    RUNDOWN,
    EVENTS_LOST,
};

struct etw_process_event {
    etw_process_event_kind kind{};

    DWORD    pid{0};
    uint64_t psn{0};

    DWORD    parent_pid{0};
    uint64_t parent_psn{0};

    FILETIME create_time{};
    FILETIME exit_time{};

    std::wstring image_name;

    // EVENTS_LOST: number of events lost in this buffer-callback window
    // (current cumulative - last observed). lost_cumulative is the running
    // total; useful for monitoring.
    uint32_t lost_count{0};
    uint32_t lost_cumulative{0};

    // For latency / observability instrumentation in the manager.
    std::chrono::steady_clock::time_point received_at{};
};

// ---------------------------------------------------------------------------
// TDH-once schema cache.
// First time a (provider, event-id, version, opcode) is seen we call
// TdhGetEventInformation to build a field plan; subsequent events of the
// same shape are decoded by walking the plan with a sliding pointer.
// ---------------------------------------------------------------------------
class tdh_process_schema_cache {
public:
    bool decode(PEVENT_RECORD record, etw_process_event_kind kind,
                etw_process_event& out) {
        out.kind = kind;
        const auto* plan = get_or_build_plan(record);
        if (!plan) return false;

        const BYTE*  base     = static_cast<const BYTE*>(record->UserData);
        const USHORT data_len = record->UserDataLength;
        const bool   is_32_bit =
            (record->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) != 0;

        USHORT cursor = 0;
        for (size_t i = 0; i < plan->fields.size(); ++i) {
            const auto& f = plan->fields[i];

            int32_t size = scalar_size_for(f.in_type, is_32_bit);
            if (size < 0) {
                size = variable_size_at(f.in_type, base, cursor, data_len);
                if (size < 0) {
                    PC_LOG_DEBUG("[etw] decode skipped: unsupported in_type={} "
                                 "field={} ev_id={} ver={}",
                                 (unsigned)f.in_type, (size_t)i,
                                 (unsigned)record->EventHeader.EventDescriptor.Id,
                                 (unsigned)record->EventHeader.EventDescriptor.Version);
                    return false;
                }
            }
            if (static_cast<USHORT>(cursor + size) > data_len) return false;

            if (i == plan->idx_pid) {
                uint32_t v;
                std::memcpy(&v, base + cursor, sizeof(v));
                out.pid = v;
            } else if (i == plan->idx_psn) {
                uint64_t v;
                std::memcpy(&v, base + cursor, sizeof(v));
                out.psn = v;
            } else if (i == plan->idx_create_time) {
                FILETIME v;
                std::memcpy(&v, base + cursor, sizeof(v));
                out.create_time = v;
            } else if (i == plan->idx_parent_pid) {
                uint32_t v;
                std::memcpy(&v, base + cursor, sizeof(v));
                out.parent_pid = v;
            } else if (i == plan->idx_parent_psn) {
                uint64_t v;
                std::memcpy(&v, base + cursor, sizeof(v));
                out.parent_psn = v;
            } else if (i == plan->idx_exit_time) {
                FILETIME v;
                std::memcpy(&v, base + cursor, sizeof(v));
                out.exit_time = v;
            } else if (i == plan->idx_image_name) {
                if (f.in_type == TDH_INTYPE_UNICODESTRING) {
                    const wchar_t* p =
                        reinterpret_cast<const wchar_t*>(base + cursor);
                    size_t max_chars =
                        (data_len - cursor) / sizeof(wchar_t);
                    size_t len = 0;
                    while (len < max_chars && p[len] != 0) ++len;
                    // ImageName is an NT-style path (\Device\HarddiskN\...\cmd.exe).
                    // Match legacy behavior: store only the basename here so
                    // tree view / list endpoints show "cmd.exe" not the full
                    // device path. The detail endpoint still calls
                    // QueryFullProcessImageNameW to get a Win32-style path.
                    const wchar_t* basename = p;
                    for (size_t k = 0; k < len; ++k) {
                        if (p[k] == L'\\') basename = p + k + 1;
                    }
                    size_t basename_len = len - (basename - p);
                    out.image_name.assign(basename, basename + basename_len);
                }
                // ProcessStop's ImageName is ANSISTRING (in_type=2). PoC
                // intentionally does not decode it — STOP only needs
                // {pid, psn} for evict.
            }

            cursor = static_cast<USHORT>(cursor + size);
        }
        return true;
    }

private:
    struct field_info {
        std::wstring name;
        USHORT in_type{0};
        USHORT out_type{0};
    };

    struct event_plan {
        std::vector<field_info> fields;
        int32_t idx_pid{-1};
        int32_t idx_psn{-1};
        int32_t idx_create_time{-1};
        int32_t idx_parent_pid{-1};
        int32_t idx_parent_psn{-1};
        int32_t idx_exit_time{-1};
        int32_t idx_image_name{-1};
    };

    struct event_key {
        GUID provider{};
        USHORT id{};
        UCHAR version{};
        UCHAR opcode{};
        bool operator==(const event_key& o) const noexcept {
            return std::memcmp(&provider, &o.provider, sizeof(GUID)) == 0 &&
                   id == o.id && version == o.version && opcode == o.opcode;
        }
    };

    struct event_key_hash {
        std::size_t operator()(const event_key& k) const noexcept {
            std::size_t h = static_cast<std::size_t>(k.provider.Data1);
            h ^= static_cast<std::size_t>(k.provider.Data2) << 1;
            h ^= static_cast<std::size_t>(k.provider.Data3) << 17;
            for (auto b : k.provider.Data4) {
                h = (h * 131u) ^ static_cast<std::size_t>(b);
            }
            h ^= static_cast<std::size_t>(k.id) << 16;
            h ^= static_cast<std::size_t>(k.version) << 8;
            h ^= static_cast<std::size_t>(k.opcode);
            return h;
        }
    };

    std::unordered_map<event_key, event_plan, event_key_hash> plans_;

    const event_plan* get_or_build_plan(PEVENT_RECORD record) {
        event_key key{
            record->EventHeader.ProviderId,
            record->EventHeader.EventDescriptor.Id,
            record->EventHeader.EventDescriptor.Version,
            record->EventHeader.EventDescriptor.Opcode,
        };
        auto it = plans_.find(key);
        if (it != plans_.end()) return &it->second;

        event_plan plan;
        if (!build_plan_with_tdh(record, plan)) return nullptr;
        auto [ins, _] = plans_.emplace(key, std::move(plan));
        return &ins->second;
    }

    bool build_plan_with_tdh(PEVENT_RECORD record, event_plan& out) {
        ULONG size = 0;
        ULONG status = TdhGetEventInformation(record, 0, nullptr, nullptr, &size);
        if (status != ERROR_INSUFFICIENT_BUFFER) {
            PC_LOG_WARN("[etw] TdhGetEventInformation size-query failed status={}", status);
            return false;
        }

        std::vector<BYTE> buf(size);
        auto* info = reinterpret_cast<TRACE_EVENT_INFO*>(buf.data());
        status = TdhGetEventInformation(record, 0, nullptr, info, &size);
        if (status != ERROR_SUCCESS) {
            PC_LOG_WARN("[etw] TdhGetEventInformation fill failed status={}", status);
            return false;
        }

        out.fields.reserve(info->TopLevelPropertyCount);
        const BYTE* info_base = reinterpret_cast<const BYTE*>(info);

        for (ULONG i = 0; i < info->TopLevelPropertyCount; ++i) {
            const auto& epi = info->EventPropertyInfoArray[i];
            field_info fi;
            if (epi.NameOffset != 0) {
                fi.name = reinterpret_cast<const wchar_t*>(info_base + epi.NameOffset);
            }
            if (epi.Flags & PropertyStruct) {
                PC_LOG_DEBUG("[etw] PropertyStruct unsupported in field name='{}'",
                             wide_to_utf8(fi.name));
                return false;
            }
            fi.in_type  = epi.nonStructType.InType;
            fi.out_type = epi.nonStructType.OutType;

            const wchar_t* nm = fi.name.c_str();
            int32_t idx = static_cast<int32_t>(i);
            if      (std::wcscmp(nm, L"ProcessID") == 0)                   out.idx_pid = idx;
            else if (std::wcscmp(nm, L"ProcessSequenceNumber") == 0)       out.idx_psn = idx;
            else if (std::wcscmp(nm, L"CreateTime") == 0)                  out.idx_create_time = idx;
            else if (std::wcscmp(nm, L"ParentProcessID") == 0)             out.idx_parent_pid = idx;
            else if (std::wcscmp(nm, L"ParentProcessSequenceNumber") == 0) out.idx_parent_psn = idx;
            else if (std::wcscmp(nm, L"ExitTime") == 0)                    out.idx_exit_time = idx;
            else if (std::wcscmp(nm, L"ImageName") == 0)                   out.idx_image_name = idx;

            out.fields.push_back(std::move(fi));
        }

        PC_LOG_DEBUG("[etw] schema plan built id={} ver={} op={} fields={} "
                     "(pid={} psn={} ct={} ppid={} ppsn={} et={} img={})",
                     (unsigned)record->EventHeader.EventDescriptor.Id,
                     (unsigned)record->EventHeader.EventDescriptor.Version,
                     (unsigned)record->EventHeader.EventDescriptor.Opcode,
                     (unsigned long)info->TopLevelPropertyCount,
                     out.idx_pid, out.idx_psn, out.idx_create_time,
                     out.idx_parent_pid, out.idx_parent_psn,
                     out.idx_exit_time, out.idx_image_name);
        return true;
    }

    static std::string wide_to_utf8(const std::wstring& w) {
        if (w.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                    static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(),
                            static_cast<int>(w.size()),
                            &s[0], n, nullptr, nullptr);
        return s;
    }

    static int32_t scalar_size_for(USHORT in_type, bool is_32_bit) {
        switch (in_type) {
            case TDH_INTYPE_INT8:
            case TDH_INTYPE_UINT8:
            case TDH_INTYPE_BOOLEAN:    return 1;
            case TDH_INTYPE_INT16:
            case TDH_INTYPE_UINT16:     return 2;
            case TDH_INTYPE_INT32:
            case TDH_INTYPE_UINT32:
            case TDH_INTYPE_HEXINT32:
            case TDH_INTYPE_FLOAT:      return 4;
            case TDH_INTYPE_INT64:
            case TDH_INTYPE_UINT64:
            case TDH_INTYPE_HEXINT64:
            case TDH_INTYPE_FILETIME:
            case TDH_INTYPE_DOUBLE:     return 8;
            case TDH_INTYPE_GUID:
            case TDH_INTYPE_SYSTEMTIME: return 16;
            case TDH_INTYPE_POINTER:
            case TDH_INTYPE_SIZET:      return is_32_bit ? 4 : 8;
            case TDH_INTYPE_UNICODESTRING:
            case TDH_INTYPE_ANSISTRING:
            case TDH_INTYPE_SID:
            case TDH_INTYPE_WBEMSID:
            case TDH_INTYPE_BINARY:     return -1;
            default:                    return -1;
        }
    }

    static int32_t variable_size_at(USHORT in_type, const BYTE* base,
                                    USHORT cursor, USHORT data_len) {
        const int32_t avail =
            static_cast<int32_t>(data_len) - static_cast<int32_t>(cursor);
        if (avail < 0) return -1;
        switch (in_type) {
            case TDH_INTYPE_UNICODESTRING: {
                const wchar_t* p =
                    reinterpret_cast<const wchar_t*>(base + cursor);
                const size_t max_chars =
                    static_cast<size_t>(avail) / sizeof(wchar_t);
                size_t len = 0;
                while (len < max_chars && p[len] != 0) ++len;
                if (len == max_chars) return avail;
                return static_cast<int32_t>((len + 1) * sizeof(wchar_t));
            }
            case TDH_INTYPE_ANSISTRING: {
                const char* p =
                    reinterpret_cast<const char*>(base + cursor);
                const size_t max_chars = static_cast<size_t>(avail);
                size_t len = strnlen(p, max_chars);
                if (len == max_chars) return avail;
                return static_cast<int32_t>(len + 1);
            }
            case TDH_INTYPE_SID: {
                if (avail < 8) return -1;
                BYTE sub_count = base[cursor + 1];
                int32_t sid_len = 8 + 4 * static_cast<int32_t>(sub_count);
                if (sid_len > avail) return -1;
                return sid_len;
            }
            case TDH_INTYPE_WBEMSID:
            case TDH_INTYPE_BINARY:
            default:
                return -1;
        }
    }
};

// ---------------------------------------------------------------------------
// ETW consumer
// ---------------------------------------------------------------------------
class etw_consumer {
public:
    using event_callback = std::function<void(const etw_process_event&)>;

    explicit etw_consumer(event_callback cb) : callback_(std::move(cb)) {}
    ~etw_consumer() { stop(); }

    etw_consumer(const etw_consumer&)            = delete;
    etw_consumer& operator=(const etw_consumer&) = delete;

    bool start() {
        if (running_) return true;

        cleanup_session();

        const size_t props_size = sizeof(EVENT_TRACE_PROPERTIES) + sizeof(SESSION_NAME);
        std::vector<std::byte> props_buf(props_size);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props_buf.data());

        props->Wnode.BufferSize    = static_cast<ULONG>(props_size);
        props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1;  // QPC clock
        props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
        props->LogFileNameOffset   = 0;  // explicit: no file output
        props->BufferSize          = 64;
        props->MinimumBuffers      = 4;
        props->MaximumBuffers      = 16;
        props->FlushTimer          = 1;

        ULONG status = StartTraceW(&session_handle_, SESSION_NAME, props);
        if (status == ERROR_ALREADY_EXISTS) {
            // Residual session — force-stop and retry once.
            PC_LOG_INFO("[etw] residual session detected, force-stopping and retrying");
            cleanup_session();
            std::memset(props_buf.data(), 0, props_buf.size());
            props->Wnode.BufferSize    = static_cast<ULONG>(props_size);
            props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
            props->Wnode.ClientContext = 1;
            props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
            props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
            props->LogFileNameOffset   = 0;
            props->BufferSize          = 64;
            props->MinimumBuffers      = 4;
            props->MaximumBuffers      = 16;
            props->FlushTimer          = 1;
            status = StartTraceW(&session_handle_, SESSION_NAME, props);
        }
        if (status != ERROR_SUCCESS) {
            PC_LOG_ERROR("[etw] StartTraceW failed status={}", status);
            return false;
        }

        // Provider-side EVENT_FILTER_TYPE_EVENT_ID restricts the kernel to
        // only emit ProcessStart/Stop/Rundown — no other Kernel-Process events
        // (ThreadStart, ImageLoad, JobStart...) reach our buffers.
        constexpr std::array<USHORT, 3> ids{
            EVENT_PROCESS_START,
            EVENT_PROCESS_STOP,
            EVENT_PROCESS_RUNDOWN,
        };
        constexpr ULONG count = 3;
        const ULONG filter_size =
            sizeof(EVENT_FILTER_EVENT_ID) + (count - 1) * sizeof(USHORT);

        std::vector<BYTE> filter_buf(filter_size);
        auto* filter = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(filter_buf.data());
        filter->FilterIn = TRUE;
        filter->Reserved = 0;
        filter->Count    = static_cast<USHORT>(count);
        for (ULONG i = 0; i < count; ++i) filter->Events[i] = ids[i];

        EVENT_FILTER_DESCRIPTOR descriptor{};
        descriptor.Ptr  = reinterpret_cast<ULONGLONG>(filter_buf.data());
        descriptor.Size = filter_size;
        descriptor.Type = EVENT_FILTER_TYPE_EVENT_ID;

        ENABLE_TRACE_PARAMETERS params{};
        params.Version          = ENABLE_TRACE_PARAMETERS_VERSION_2;
        params.EnableFilterDesc = &descriptor;
        params.FilterDescCount  = 1;

        status = EnableTraceEx2(
            session_handle_,
            &KERNEL_PROCESS_GUID,
            EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            TRACE_LEVEL_INFORMATION,
            KEYWORD_PROCESS,
            0, 0, &params);
        if (status != ERROR_SUCCESS) {
            PC_LOG_ERROR("[etw] EnableTraceEx2 ENABLE failed status={}", status);
            stop_session();
            return false;
        }

        std::memset(&logfile_, 0, sizeof(logfile_));
        logfile_.LoggerName        = SESSION_NAME;
        logfile_.ProcessTraceMode  = PROCESS_TRACE_MODE_REAL_TIME |
                                     PROCESS_TRACE_MODE_EVENT_RECORD;
        logfile_.Context           = this;
        logfile_.EventRecordCallback = event_record_callback;
        logfile_.BufferCallback    = buffer_callback;

        trace_handle_ = OpenTraceW(&logfile_);
        if (trace_handle_ == INVALID_PROCESSTRACE_HANDLE) {
            PC_LOG_ERROR("[etw] OpenTraceW failed err={}", GetLastError());
            stop_session();
            return false;
        }

        running_ = true;
        const TRACEHANDLE consumer_handle = trace_handle_;
        trace_thread_ = std::jthread([this, consumer_handle]() {
            TRACEHANDLE h = consumer_handle;
            ULONG rc = ProcessTrace(&h, 1, nullptr, nullptr);
            if (rc != ERROR_SUCCESS && rc != ERROR_CANCELLED && running_) {
                PC_LOG_WARN("[etw] ProcessTrace returned {}", rc);
            }
        });

        PC_LOG_INFO("[etw] consumer started (provider=Microsoft-Windows-Kernel-Process, filter={{1,2,15}})");
        return true;
    }

    // Trigger ProcessRundown(15) emission for currently-live processes.
    // Same session, same provider — no new TRACEHANDLE needed.
    void request_rundown() {
        if (!running_) return;
        const ULONG status = EnableTraceEx2(
            session_handle_,
            &KERNEL_PROCESS_GUID,
            EVENT_CONTROL_CODE_CAPTURE_STATE,
            TRACE_LEVEL_INFORMATION,
            KEYWORD_PROCESS,
            0, 0, nullptr);
        if (status != ERROR_SUCCESS) {
            PC_LOG_WARN("[etw] CAPTURE_STATE failed status={}", status);
        } else {
            PC_LOG_INFO("[etw] CAPTURE_STATE issued");
        }
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
        PC_LOG_INFO("[etw] consumer stopped");
    }

    bool is_running() const { return running_; }

private:
    static constexpr GUID KERNEL_PROCESS_GUID = {
        0x22fb2cd6, 0x0e7b, 0x422b,
        { 0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16 }
    };
    static constexpr ULONGLONG KEYWORD_PROCESS = 0x10;
    static constexpr USHORT EVENT_PROCESS_START   = 1;
    static constexpr USHORT EVENT_PROCESS_STOP    = 2;
    static constexpr USHORT EVENT_PROCESS_RUNDOWN = 15;

    // Session name change: ClewETW -> ClewProcessEtw forces any leftover
    // session from earlier builds to be cleaned via ERROR_ALREADY_EXISTS path.
    static inline wchar_t SESSION_NAME[] = L"ClewProcessEtw";

    event_callback             callback_;
    std::atomic<bool>          running_{false};
    TRACEHANDLE                session_handle_{0};
    TRACEHANDLE                trace_handle_{INVALID_PROCESSTRACE_HANDLE};
    EVENT_TRACE_LOGFILEW       logfile_{};
    std::jthread               trace_thread_;
    tdh_process_schema_cache   schema_cache_;
    std::atomic<ULONG>         last_events_lost_{0};

    static void WINAPI event_record_callback(PEVENT_RECORD record) {
        auto* self = static_cast<etw_consumer*>(record->UserContext);
        if (!self || !self->running_) return;

        if (std::memcmp(&record->EventHeader.ProviderId,
                        &KERNEL_PROCESS_GUID, sizeof(GUID)) != 0) {
            return;
        }

        const auto id = record->EventHeader.EventDescriptor.Id;
        etw_process_event_kind kind;
        if (id == EVENT_PROCESS_START)        kind = etw_process_event_kind::START;
        else if (id == EVENT_PROCESS_STOP)    kind = etw_process_event_kind::STOP;
        else if (id == EVENT_PROCESS_RUNDOWN) kind = etw_process_event_kind::RUNDOWN;
        else return;

        etw_process_event evt;
        evt.received_at = std::chrono::steady_clock::now();
        if (!self->schema_cache_.decode(record, kind, evt)) return;

        self->callback_(evt);
    }

    static ULONG WINAPI buffer_callback(PEVENT_TRACE_LOGFILEW logfile) {
        auto* self = static_cast<etw_consumer*>(logfile->Context);
        if (!self) return FALSE;

        const ULONG current  = logfile->EventsLost;
        const ULONG previous = self->last_events_lost_.load(std::memory_order_relaxed);
        if (current > previous) {
            self->last_events_lost_.store(current, std::memory_order_relaxed);
            etw_process_event evt;
            evt.kind            = etw_process_event_kind::EVENTS_LOST;
            evt.lost_count      = current - previous;
            evt.lost_cumulative = current;
            evt.received_at     = std::chrono::steady_clock::now();
            self->callback_(evt);
        }

        return self->running_ ? TRUE : FALSE;
    }

    void cleanup_session() noexcept {
        try {
            const size_t buf_size =
                sizeof(EVENT_TRACE_PROPERTIES) + sizeof(SESSION_NAME);
            std::vector<std::byte> props_buf(buf_size);
            auto* props =
                reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props_buf.data());
            props->Wnode.BufferSize  = static_cast<ULONG>(buf_size);
            props->LoggerNameOffset  = sizeof(EVENT_TRACE_PROPERTIES);
            props->LogFileNameOffset = 0;
            ControlTraceW(0, SESSION_NAME, props, EVENT_TRACE_CONTROL_STOP);
            // Also try old session name from pre-refactor builds.
            std::vector<std::byte> old_buf(sizeof(EVENT_TRACE_PROPERTIES) +
                                           sizeof(L"ClewETW"));
            auto* op = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(old_buf.data());
            op->Wnode.BufferSize  = static_cast<ULONG>(old_buf.size());
            op->LoggerNameOffset  = sizeof(EVENT_TRACE_PROPERTIES);
            op->LogFileNameOffset = 0;
            ControlTraceW(0, L"ClewETW", op, EVENT_TRACE_CONTROL_STOP);
        } catch (const std::bad_alloc&) {
            PC_LOG_WARN("[etw] cleanup_session OOM, skipping orphan reap");
        }
    }

    void stop_session() noexcept {
        if (session_handle_ == 0) return;
        EnableTraceEx2(session_handle_, &KERNEL_PROCESS_GUID,
                       EVENT_CONTROL_CODE_DISABLE_PROVIDER,
                       0, 0, 0, 0, nullptr);
        try {
            const size_t buf_size =
                sizeof(EVENT_TRACE_PROPERTIES) + sizeof(SESSION_NAME);
            std::vector<std::byte> props_buf(buf_size);
            auto* props =
                reinterpret_cast<EVENT_TRACE_PROPERTIES*>(props_buf.data());
            props->Wnode.BufferSize  = static_cast<ULONG>(buf_size);
            props->LoggerNameOffset  = sizeof(EVENT_TRACE_PROPERTIES);
            props->LogFileNameOffset = 0;
            ControlTraceW(session_handle_, nullptr, props, EVENT_TRACE_CONTROL_STOP);
        } catch (const std::bad_alloc&) {
            PC_LOG_WARN("[etw] stop_session OOM");
        }
        session_handle_ = 0;
    }
};

} // namespace clew
