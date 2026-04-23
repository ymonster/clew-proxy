#pragma once

// NtQuerySystemInformation wrapper for process enumeration.
// Returns vector<raw_process_record> with PID, parent PID, CreateTime, image name.
// Single syscall, 0ms latency, provides CreateTime (TH32 does not).
//
// PoC lessons (Phase 2b):
//   - Windows SDK winternl.h has stripped SYSTEM_PROCESS_INFORMATION → custom struct needed
//   - ntdll.lib static linking works on MSVC
//   - CreateTime is LARGE_INTEGER, binary-compatible with FILETIME

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <vector>
#include <cstring>
#include "core/log.hpp"

#include "process/flat_tree.hpp"  // for raw_process_record

#pragma comment(lib, "ntdll.lib")

namespace clew {

// Full SYSTEM_PROCESS_INFORMATION (SDK's winternl.h is truncated)
struct ntquery_unicode_string {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct ntquery_system_process_info {
    ULONG             NextEntryOffset;
    ULONG             NumberOfThreads;
    LARGE_INTEGER     WorkingSetPrivateSize;
    ULONG             HardFaultCount;
    ULONG             NumberOfThreadsHighWatermark;
    ULONGLONG         CycleTime;
    LARGE_INTEGER     CreateTime;
    LARGE_INTEGER     UserTime;
    LARGE_INTEGER     KernelTime;
    ntquery_unicode_string ImageName;
    LONG              BasePriority;
    HANDLE            UniqueProcessId;
    HANDLE            InheritedFromUniqueProcessId;
    ULONG             HandleCount;
    ULONG             SessionId;
    ULONG_PTR         UniqueProcessKey;
    SIZE_T            PeakVirtualSize;
    SIZE_T            VirtualSize;
    ULONG             PageFaultCount;
    SIZE_T            PeakWorkingSetSize;
    SIZE_T            WorkingSetSize;
    SIZE_T            QuotaPeakPagedPoolUsage;
    SIZE_T            QuotaPagedPoolUsage;
    SIZE_T            QuotaPeakNonPagedPoolUsage;
    SIZE_T            QuotaNonPagedPoolUsage;
    SIZE_T            PagefileUsage;
    SIZE_T            PeakPagefileUsage;
    SIZE_T            PrivatePageCount;
    LARGE_INTEGER     ReadOperationCount;
    LARGE_INTEGER     WriteOperationCount;
    LARGE_INTEGER     OtherOperationCount;
    LARGE_INTEGER     ReadTransferCount;
    LARGE_INTEGER     WriteTransferCount;
    LARGE_INTEGER     OtherTransferCount;
};

// Use GetProcAddress to avoid conflict with winternl.h declaration
using NtQuerySystemInformation_fn = LONG(NTAPI*)(
    ULONG, PVOID, ULONG, PULONG);

inline NtQuerySystemInformation_fn get_ntquery_fn() {
    static auto fn = reinterpret_cast<NtQuerySystemInformation_fn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    return fn;
}

inline std::vector<raw_process_record> ntquery_enumerate_processes() {
    constexpr ULONG SystemProcessInformation = 5;
    constexpr LONG STATUS_INFO_LENGTH_MISMATCH = static_cast<LONG>(0xC0000004);

    std::vector<raw_process_record> result;
    ULONG buf_size = 1024 * 1024;  // 1MB initial
    std::vector<BYTE> buffer(buf_size);
    ULONG returned = 0;

    LONG status;
    for (int attempt = 0; attempt < 5; attempt++) {
        auto NtQuerySysInfo = get_ntquery_fn();
        if (!NtQuerySysInfo) {
            PC_LOG_ERROR("[NtQuery] Failed to resolve NtQuerySystemInformation");
            return result;
        }
        status = NtQuerySysInfo(
            SystemProcessInformation, buffer.data(), buf_size, &returned);
        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            buf_size = returned + 65536;
            buffer.resize(buf_size);
            continue;
        }
        break;
    }

    if (status != 0) {
        PC_LOG_ERROR("[NtQuery] Failed: 0x{:08X}", static_cast<unsigned>(status));
        return result;
    }

    auto* entry = reinterpret_cast<ntquery_system_process_info*>(buffer.data());
    result.reserve(512);

    while (true) {
        raw_process_record rec;
        rec.pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(entry->UniqueProcessId));
        rec.parent_pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(entry->InheritedFromUniqueProcessId));
        rec.create_time = std::bit_cast<FILETIME>(entry->CreateTime);

        if (entry->ImageName.Buffer && entry->ImageName.Length > 0) {
            int len = entry->ImageName.Length / sizeof(wchar_t);
            if (len > 259) len = 259;
            wcsncpy_s(rec.name, std::size(rec.name), entry->ImageName.Buffer, len);
        } else {
            wcscpy_s(rec.name, std::size(rec.name),
                     (rec.pid == 0) ? L"System Idle Process" : L"System");
        }

        result.push_back(rec);

        if (entry->NextEntryOffset == 0) break;
        entry = reinterpret_cast<ntquery_system_process_info*>(
            reinterpret_cast<BYTE*>(entry) + entry->NextEntryOffset);
    }

    PC_LOG_INFO("[NtQuery] Enumerated {} processes", result.size());
    return result;
}

} // namespace clew
