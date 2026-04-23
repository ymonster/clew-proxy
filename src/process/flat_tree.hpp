#pragma once

// Flat Tree: contiguous-memory process tree with LC-RS (Left-Child Right-Sibling)
// representation. All mutations happen on a single strand — no locks needed.
//
// Part of Clew v2 backend restructuring (Phase 1).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace clew {

static constexpr uint32_t NO_PROXY    = UINT32_MAX;
static constexpr uint32_t INVALID_IDX = UINT32_MAX;

// Flags stored in process_entry::flags
namespace entry_flags {
    static constexpr uint8_t MANUAL_HIJACK  = 0x01;  // bit 0
    static constexpr uint8_t AUTO_MATCHED   = 0x02;  // bit 1
    static constexpr uint8_t EXCLUDED       = 0x04;  // bit 2
}

struct process_entry {
    DWORD    pid            = 0;
    DWORD    parent_pid     = 0;
    FILETIME create_time    = {};           // PID reuse detection + ETW dedup

    // LC-RS tree indices
    uint32_t parent_index       = INVALID_IDX;
    uint32_t first_child_index  = INVALID_IDX;
    uint32_t next_sibling_index = INVALID_IDX;

    // Process name in UTF-8, converted once from wchar_t at entry time
    char     name_u8[780]   = {};           // MAX_PATH * 3 bytes for UTF-8

    bool     alive          = true;

    // Proxy state: group id, NO_PROXY = not proxied
    uint32_t group_id       = NO_PROXY;
    uint8_t  flags          = 0;

    // Lazy cmdline cache: empty = not yet queried, "\x01" = queried but failed/empty
    std::string cmdline_cache;
    std::string image_path_cache;  // same convention as cmdline_cache

    // Helper: set name from wide string (one-time conversion at entry)
    void set_name(const wchar_t* wide_name) {
        if (!wide_name || wide_name[0] == L'\0') {
            name_u8[0] = '\0';
            return;
        }
        int len = WideCharToMultiByte(CP_UTF8, 0, wide_name, -1,
                                       name_u8, sizeof(name_u8), nullptr, nullptr);
        if (len <= 0) {
            name_u8[0] = '\0';
        }
    }

    // Helper: set name from narrow string directly
    void set_name(const char* utf8_name) {
        if (!utf8_name || utf8_name[0] == '\0') {
            name_u8[0] = '\0';
            return;
        }
        strncpy_s(name_u8, sizeof(name_u8), utf8_name, _TRUNCATE);
    }

    bool is_proxied() const { return group_id != NO_PROXY; }

    bool has_flag(uint8_t f) const { return (flags & f) != 0; }
    void set_flag(uint8_t f)   { flags |= f; }
    void clear_flag(uint8_t f) { flags &= ~f; }
};

// Query process command line from OS via NtQueryInformationProcess.
// Returns empty string on failure (access denied, process exited, etc.)
inline std::string query_process_cmdline(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h || h == INVALID_HANDLE_VALUE) return {};

    std::string cmdline;

    // NtQueryInformationProcess signature — use LONG instead of NTSTATUS to avoid winternl.h dependency
    using NtQueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto NtQuery = reinterpret_cast<NtQueryFn>(
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess"));
    if (NtQuery) {
        ULONG buf_size = 1024;
        auto buf = std::make_unique<uint8_t[]>(buf_size);
        ULONG ret_len = 0;
        LONG status = NtQuery(h, 60, buf.get(), buf_size, &ret_len);
        if (status == static_cast<LONG>(0xC0000004) /* STATUS_INFO_LENGTH_MISMATCH */ && ret_len > 0) {
            buf_size = ret_len;
            buf = std::make_unique<uint8_t[]>(buf_size);
            status = NtQuery(h, 60, buf.get(), buf_size, &ret_len);
        }
        if (status >= 0) { // NT_SUCCESS
            const struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } *us =
                reinterpret_cast<decltype(us)>(buf.get());
            if (us->Buffer && us->Length > 0) {
                int wchar_count = us->Length / sizeof(WCHAR);
                int utf8_len = WideCharToMultiByte(CP_UTF8, 0, us->Buffer, wchar_count,
                                                   nullptr, 0, nullptr, nullptr);
                if (utf8_len > 0) {
                    cmdline.resize(utf8_len);
                    WideCharToMultiByte(CP_UTF8, 0, us->Buffer, wchar_count,
                                       cmdline.data(), utf8_len, nullptr, nullptr);
                }
            }
        }
    }

    CloseHandle(h);
    return cmdline;
}

// Query process image path via QueryFullProcessImageNameW.
// Returns empty string on failure.
inline std::string query_process_image_path(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h || h == INVALID_HANDLE_VALUE) return {};
    WCHAR buf[MAX_PATH];
    DWORD len = MAX_PATH;
    std::string result;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buf, len, nullptr, 0, nullptr, nullptr);
        if (utf8_len > 0) {
            result.resize(utf8_len);
            WideCharToMultiByte(CP_UTF8, 0, buf, len, result.data(), utf8_len, nullptr, nullptr);
        }
    }
    CloseHandle(h);
    return result;
}

// Raw process record from NtQuerySystemInformation snapshot
struct raw_process_record {
    DWORD    pid;
    DWORD    parent_pid;
    FILETIME create_time;
    wchar_t  name[260];     // Wide string from OS
};

class flat_tree {
public:
    flat_tree() = default;
    flat_tree(const flat_tree&) = default;
    flat_tree& operator=(const flat_tree&) = default;
    flat_tree(flat_tree&&) noexcept = default;
    flat_tree& operator=(flat_tree&&) noexcept = default;

    // ---- Accessors (strand-safe, only called from strand context) ----

    const std::vector<process_entry>& entries() const { return entries_; }
    std::vector<process_entry>& entries() { return entries_; }

    uint32_t alive_count() const { return alive_count_; }
    uint32_t tombstone_count() const { return tombstone_count_; }

    // Find entry index by PID. Returns INVALID_IDX if not found.
    uint32_t find_by_pid(DWORD pid) const {
        auto it = side_map_.find(pid);
        return (it != side_map_.end()) ? it->second : INVALID_IDX;
    }

    // Direct entry access by index
    const process_entry& at(uint32_t idx) const { return entries_[idx]; }
    process_entry& at(uint32_t idx) { return entries_[idx]; }

    // ---- Build / Modify (strand-safe) ----

    // Build initial tree from NtQuery snapshot
    void build_from_snapshot(const std::vector<raw_process_record>& records) {
        entries_.clear();
        side_map_.clear();
        entries_.reserve(records.size());

        for (const auto& r : records) {
            process_entry e;
            e.pid = r.pid;
            e.parent_pid = r.parent_pid;
            e.create_time = r.create_time;
            e.set_name(r.name);
            e.alive = true;

            uint32_t idx = static_cast<uint32_t>(entries_.size());
            entries_.push_back(std::move(e));
            side_map_[r.pid] = idx;
        }

        alive_count_ = static_cast<uint32_t>(entries_.size());
        tombstone_count_ = 0;

        rebuild_lc_rs_links();
    }

    // Add a new process entry (ETW ProcessStart)
    // Returns the index of the new entry.
    uint32_t add_entry(DWORD pid, DWORD parent_pid, FILETIME create_time,
                       const wchar_t* name) {
        // Check for PID reuse
        auto it = side_map_.find(pid);
        if (it != side_map_.end()) {
            const auto& existing = entries_[it->second];
            if (filetime_equal(existing.create_time, create_time)) {
                return it->second;  // Idempotent: same process, skip
            }
            // PID reuse: tombstone the old entry
            tombstone_entry(it->second);
        }

        process_entry e;
        e.pid = pid;
        e.parent_pid = parent_pid;
        e.create_time = create_time;
        e.set_name(name);
        e.alive = true;

        uint32_t idx = static_cast<uint32_t>(entries_.size());
        entries_.push_back(std::move(e));
        side_map_[pid] = idx;
        alive_count_++;

        // Link into parent's child list
        link_to_parent(idx);

        return idx;
    }

    // Mark entry as tombstone (ETW ProcessStop)
    // Returns true if entry was tombstoned, false if not found or create_time mismatch.
    bool tombstone(DWORD pid, FILETIME create_time) {
        auto it = side_map_.find(pid);
        if (it == side_map_.end()) return false;

        const auto& entry = entries_[it->second];
        if (!filetime_equal(entry.create_time, create_time)) return false;

        tombstone_entry(it->second);
        check_compact();
        return true;
    }

    // Tombstone by index (no create_time check, used internally)
    void tombstone_entry(uint32_t idx) {
        auto& entry = entries_[idx];
        if (!entry.alive) return;

        // Reparent alive children to nearest alive ancestor.
        // Walk up from dying entry's parent_index to find first alive ancestor.
        // If none found, children become roots (parent_index = INVALID_IDX).
        reparent_children(idx);

        entry.alive = false;
        entry.group_id = NO_PROXY;
        entry.flags = 0;
        alive_count_--;
        tombstone_count_++;
        side_map_.erase(entry.pid);
    }

    // Compact: remove tombstones, rebuild indices
    void compact() {
        // Step 1: remove dead entries
        std::erase_if(entries_, [](const process_entry& e) { return !e.alive; });

        // Step 2: rebuild side_map (indices shifted)
        side_map_.clear();
        for (uint32_t i = 0; i < entries_.size(); i++) {
            side_map_[entries_[i].pid] = i;
        }

        // Step 3: rebuild LC-RS links
        rebuild_lc_rs_links();

        alive_count_ = static_cast<uint32_t>(entries_.size());
        tombstone_count_ = 0;
    }

    // Rebuild LC-RS links from parent_pid fields (O(N) pass)
    // Used by both build_from_snapshot and compact
    void rebuild_lc_rs_links() {
        // Reset all links
        for (auto& e : entries_) {
            e.parent_index = INVALID_IDX;
            e.first_child_index = INVALID_IDX;
            e.next_sibling_index = INVALID_IDX;
        }

        // Build links: for each entry, find parent and prepend to child list
        for (uint32_t i = 0; i < entries_.size(); i++) {
            if (!entries_[i].alive) continue;

            auto pit = side_map_.find(entries_[i].parent_pid);
            if (pit != side_map_.end() && pit->second != i) {
                uint32_t parent_idx = pit->second;
                if (entries_[parent_idx].alive) {
                    entries_[i].parent_index = parent_idx;
                    // Prepend to parent's child list
                    entries_[i].next_sibling_index = entries_[parent_idx].first_child_index;
                    entries_[parent_idx].first_child_index = i;
                }
            }
        }
    }

    // ---- Tree Traversal (strand-safe) ----

    // Visit all descendants of entry at `idx` (DFS via LC-RS)
    template<typename Fn>
    void visit_descendants(uint32_t idx, Fn&& fn) const {
        uint32_t child = entries_[idx].first_child_index;
        while (child != INVALID_IDX) {
            if (entries_[child].alive) {
                fn(child, entries_[child]);
                visit_descendants(child, fn);
            }
            child = entries_[child].next_sibling_index;
        }
    }

    // Get root entries (entries with no alive parent)
    std::vector<uint32_t> get_roots() const {
        std::vector<uint32_t> roots;
        for (uint32_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].alive && entries_[i].parent_index == INVALID_IDX) {
                roots.push_back(i);
            }
        }
        return roots;
    }

    // ---- JSON Serialization (for API snapshot) ----

    // Serialize entire tree to JSON string
    std::string to_json() const {
        nlohmann::json arr = nlohmann::json::array();
        auto roots = get_roots();
        for (uint32_t idx : roots) {
            arr.push_back(entry_to_json(idx));
        }
        return arr.dump();
    }

    // ---- Side map access ----

    const std::unordered_map<DWORD, uint32_t>& side_map() const { return side_map_; }

private:
    std::vector<process_entry>                entries_;
    std::unordered_map<DWORD, uint32_t>       side_map_;   // pid → entries_ index
    uint32_t                                  alive_count_{0};
    uint32_t                                  tombstone_count_{0};

    // Check if compaction is needed (tombstones > 20% of alive)
    void check_compact() {
        if (alive_count_ > 0 && tombstone_count_ > alive_count_ / 5) {
            compact();
        }
    }

    // Reparent alive children of a dying entry to its nearest alive ancestor.
    // Example: explorer → A → B → C, A dies → B reparents to explorer.
    // If no alive ancestor exists, children become roots.
    void reparent_children(uint32_t dying_idx) {
        // Find nearest alive ancestor by walking up parent_index chain
        uint32_t ancestor = entries_[dying_idx].parent_index;
        while (ancestor != INVALID_IDX && !entries_[ancestor].alive) {
            ancestor = entries_[ancestor].parent_index;
        }

        // Reparent each alive child
        uint32_t child = entries_[dying_idx].first_child_index;
        while (child != INVALID_IDX) {
            uint32_t next = entries_[child].next_sibling_index;
            if (entries_[child].alive) {
                // Detach from dying parent, attach to ancestor
                entries_[child].parent_index = ancestor;
                entries_[child].parent_pid = (ancestor != INVALID_IDX)
                    ? entries_[ancestor].pid : 0;
                if (ancestor != INVALID_IDX) {
                    // Prepend to ancestor's child list
                    entries_[child].next_sibling_index = entries_[ancestor].first_child_index;
                    entries_[ancestor].first_child_index = child;
                } else {
                    // No alive ancestor — becomes root
                    entries_[child].next_sibling_index = INVALID_IDX;
                }
            }
            child = next;
        }
    }

    // Link a new entry into its parent's child list
    void link_to_parent(uint32_t idx) {
        auto& entry = entries_[idx];
        auto pit = side_map_.find(entry.parent_pid);
        if (pit != side_map_.end() && pit->second != idx) {
            uint32_t parent_idx = pit->second;
            if (entries_[parent_idx].alive) {
                entry.parent_index = parent_idx;
                entry.next_sibling_index = entries_[parent_idx].first_child_index;
                entries_[parent_idx].first_child_index = idx;
            }
        }
    }

    // FILETIME comparison
    static bool filetime_equal(const FILETIME& a, const FILETIME& b) {
        return a.dwLowDateTime == b.dwLowDateTime &&
               a.dwHighDateTime == b.dwHighDateTime;
    }

    // Recursive JSON serialization for a single entry + children
    nlohmann::json entry_to_json(uint32_t idx) const {
        const auto& e = entries_[idx];
        nlohmann::json j;
        j["pid"] = e.pid;
        j["parent_pid"] = e.parent_pid;
        j["name"] = e.name_u8;
        j["proxied"] = e.is_proxied();

        if (e.group_id != NO_PROXY) {
            j["group_id"] = e.group_id;
        }
        if (e.flags != 0) {
            j["manual"] = e.has_flag(entry_flags::MANUAL_HIJACK);
            j["auto_matched"] = e.has_flag(entry_flags::AUTO_MATCHED);
        }

        nlohmann::json children = nlohmann::json::array();
        uint32_t child = e.first_child_index;
        while (child != INVALID_IDX) {
            if (entries_[child].alive) {
                children.push_back(entry_to_json(child));
            }
            child = entries_[child].next_sibling_index;
        }

        if (!children.empty()) {
            j["children"] = std::move(children);
        }

        return j;
    }
};

} // namespace clew
