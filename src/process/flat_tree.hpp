#pragma once

// Flat Tree: contiguous-memory process tree with LC-RS (Left-Child Right-Sibling)
// representation. All mutations happen on a single strand — no locks needed.
//
// PSN-keyed disambiguation (post-NtQuery refactor):
//   - process_entry stores PSN (ProcessSequenceNumber) and parent_psn from
//     Microsoft-Windows-Kernel-Process. PSN is boot-unique + monotonic, so
//     PID reuse never produces ambiguous (pid, psn) pairs.
//   - side_map maps PID -> {index, psn}; the psn member is a tag used to
//     reject stale STOP events that target a since-recycled PID.
//   - Parent linkage is requested explicitly by the manager; flat_tree only
//     provides primitives (attach_child, mark_root, detach_from_parent).
//   - The manager owns orphan tracking — flat_tree never "looks up" a parent
//     on its own.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/scoped_exit.hpp"

namespace clew {

static constexpr uint32_t NO_PROXY    = UINT32_MAX;
static constexpr uint32_t INVALID_IDX = UINT32_MAX;

// PSN sentinels.
//
// ROOT_PSN_SENTINEL: ETW emits 0xFFFFFFFFFFFFFFFF as the parent PSN for
// processes whose parent doesn't exist (Idle, System) or whose parent died
// before PSN was assigned (very early csrss instances). The manager treats
// this value as "attach to root, never look up a parent".
//
// INVALID_PSN: 0 — used as "PSN unknown / not yet observed". Real PSNs start
// at 1 (Idle is 0 / System is 1, but we treat Idle's PSN=0 as ROOT_PSN_SENTINEL
// since it has no meaningful parent).
static constexpr uint64_t ROOT_PSN_SENTINEL = 0xFFFFFFFFFFFFFFFFULL;
static constexpr uint64_t INVALID_PSN       = 0;

// Flags stored in process_entry::flags
namespace entry_flags {
    static constexpr uint8_t MANUAL_HIJACK  = 0x01;  // bit 0
    static constexpr uint8_t AUTO_MATCHED   = 0x02;  // bit 1
    static constexpr uint8_t EXCLUDED       = 0x04;  // bit 2
}

struct process_entry {
    DWORD    pid            = 0;
    DWORD    parent_pid     = 0;
    FILETIME create_time    = {};           // Retained for diagnostics + future
                                            // PSN-less environment fallback

    // PSN-based disambiguation (post-refactor primary identity)
    uint64_t psn            = INVALID_PSN;        // boot-unique, monotonic
    uint64_t parent_psn     = ROOT_PSN_SENTINEL;  // 0xFFFF...F = no parent

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
    auto h = wrap_handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return {};

    using NtQueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static auto NtQuery = reinterpret_cast<NtQueryFn>(
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess"));
    if (!NtQuery) return {};

    ULONG buf_size = 1024;
    auto buf = std::make_unique<uint8_t[]>(buf_size);
    ULONG ret_len = 0;
    LONG status = NtQuery(h.get(), 60, buf.get(), buf_size, &ret_len);
    if (status == static_cast<LONG>(0xC0000004) /* STATUS_INFO_LENGTH_MISMATCH */ && ret_len > 0) {
        buf_size = ret_len;
        buf = std::make_unique<uint8_t[]>(buf_size);
        status = NtQuery(h.get(), 60, buf.get(), buf_size, &ret_len);
    }
    if (status < 0) return {};

    const struct { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } *us =
        reinterpret_cast<decltype(us)>(buf.get());
    if (!us->Buffer || us->Length == 0) return {};

    int wchar_count = us->Length / sizeof(WCHAR);
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, us->Buffer, wchar_count,
                                       nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return {};

    std::string cmdline(utf8_len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, us->Buffer, wchar_count,
                        cmdline.data(), utf8_len, nullptr, nullptr);
    return cmdline;
}

// Query process image path via QueryFullProcessImageNameW.
// Returns empty string on failure.
inline std::string query_process_image_path(DWORD pid) {
    auto h = wrap_handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h) return {};

    WCHAR buf[MAX_PATH];
    DWORD len = MAX_PATH;
    if (!QueryFullProcessImageNameW(h.get(), 0, buf, &len)) return {};

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buf, len, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return {};

    std::string result(utf8_len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, len, result.data(), utf8_len, nullptr, nullptr);
    return result;
}

// Side-map value: index into entries_ + PSN tag for disambiguation.
struct side_entry {
    uint32_t index;
    uint64_t psn;
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
    // Hot-path query (windivert SOCKET layer): no PSN check, latest PID
    // owner wins.
    uint32_t find_by_pid(DWORD pid) const {
        auto it = side_map_.find(pid);
        return (it != side_map_.end()) ? it->second.index : INVALID_IDX;
    }

    // Find with PSN verification. Returns INVALID_IDX if PID absent OR if
    // the side_map entry's PSN differs from expected_psn (= the side_map
    // is now owned by a recycled PID's new entry, this lookup is stale).
    // Used by STOP-event evict and by parent-resolution paths.
    uint32_t find_by_pid_psn(DWORD pid, uint64_t expected_psn) const {
        auto it = side_map_.find(pid);
        if (it == side_map_.end()) return INVALID_IDX;
        if (it->second.psn != expected_psn) return INVALID_IDX;
        return it->second.index;
    }

    // Direct entry access by index. Hot path (callers have already
    // verified idx via find_by_pid). Debug build asserts; release build
    // is unchecked for backward compatibility.
    const process_entry& at(uint32_t idx) const {
        assert(idx < entries_.size());
        return entries_[idx];
    }
    process_entry& at(uint32_t idx) {
        assert(idx < entries_.size());
        return entries_[idx];
    }

    // Bounds-safe accessor for HTTP API paths where idx may be stale
    // (PID-reuse race + service layer indirection). Returns nullptr on
    // out-of-bounds. Resolves the wininit-1032 0xc0000005 + 0xffffbaad
    // crash that happens when find_by_pid races against tombstone_entry.
    const process_entry* try_at(uint32_t idx) const noexcept {
        if (idx >= entries_.size()) return nullptr;
        return &entries_[idx];
    }

    // ---- Build / Modify (strand-safe) ----

    // Add a new process entry. Caller owns parent linkage — call
    // attach_child or mark_root after add_entry as appropriate.
    //
    // Idempotency: if (pid, psn) already exists, returns existing index
    // without modification. If pid exists but psn differs (rare: missed
    // STOP for old entry + lost handler hasn't recovered yet), the old
    // entry is tombstoned first.
    uint32_t add_entry(DWORD pid,
                       DWORD parent_pid,
                       uint64_t psn,
                       uint64_t parent_psn,
                       FILETIME create_time,
                       const wchar_t* name) {
        if (auto it = side_map_.find(pid); it != side_map_.end()) {
            if (it->second.psn == psn) {
                return it->second.index;  // idempotent
            }
            // PID-reuse / lost STOP. Tombstone old before installing new.
            tombstone_entry(it->second.index);
        }

        process_entry e;
        e.pid          = pid;
        e.parent_pid   = parent_pid;
        e.psn          = psn;
        e.parent_psn   = parent_psn;
        e.create_time  = create_time;
        e.set_name(name);
        e.alive        = true;

        uint32_t idx = static_cast<uint32_t>(entries_.size());
        entries_.push_back(std::move(e));
        side_map_[pid] = side_entry{idx, psn};
        alive_count_++;

        return idx;
    }

    // Attach child_idx as a child of parent_idx. Updates child.parent_index
    // and prepends child to parent's first_child_index list.
    // Caller verified parent_idx is valid + alive.
    void attach_child(uint32_t parent_idx, uint32_t child_idx) {
        assert(parent_idx < entries_.size());
        assert(child_idx  < entries_.size());
        assert(parent_idx != child_idx);

        auto& child  = entries_[child_idx];
        auto& parent = entries_[parent_idx];

        // Defensive: detach from any prior parent before re-attach
        detach_from_parent(child_idx);

        child.parent_index       = parent_idx;
        child.next_sibling_index = parent.first_child_index;
        parent.first_child_index = child_idx;
    }

    // Mark idx as a root (no parent). Detaches from any current parent.
    void mark_root(uint32_t idx) {
        detach_from_parent(idx);
        entries_[idx].parent_index       = INVALID_IDX;
        entries_[idx].next_sibling_index = INVALID_IDX;
    }

    // Detach an entry from its current parent's child list. Does NOT modify
    // parent_pid / parent_psn (those are facts; only LC-RS link is touched).
    void detach_from_parent(uint32_t idx) {
        auto& child = entries_[idx];
        if (child.parent_index == INVALID_IDX) return;

        auto& parent = entries_[child.parent_index];
        if (parent.first_child_index == idx) {
            parent.first_child_index = child.next_sibling_index;
        } else {
            uint32_t cur = parent.first_child_index;
            while (cur != INVALID_IDX) {
                auto& s = entries_[cur];
                if (s.next_sibling_index == idx) {
                    s.next_sibling_index = child.next_sibling_index;
                    break;
                }
                cur = s.next_sibling_index;
            }
        }
        child.parent_index       = INVALID_IDX;
        child.next_sibling_index = INVALID_IDX;
    }

    // Mark entry as tombstone with PSN verification.
    // Returns true if entry was tombstoned, false if not found or stale
    // (PSN doesn't match). Stale STOPs are silently ignored.
    bool tombstone(DWORD pid, uint64_t psn) {
        auto it = side_map_.find(pid);
        if (it == side_map_.end()) return false;
        if (it->second.psn != psn) return false;
        tombstone_entry(it->second.index);
        check_compact();
        return true;
    }

    // Tombstone by index (no verification — caller is internal).
    void tombstone_entry(uint32_t idx) {
        auto& entry = entries_[idx];
        if (!entry.alive) return;

        // Reparent alive children to nearest alive ancestor.
        reparent_children(idx);

        // Remove from parent's child list (we may have reattached siblings
        // above, but our own slot still needs detaching).
        detach_from_parent(idx);

        entry.alive    = false;
        entry.group_id = NO_PROXY;
        entry.flags    = 0;
        alive_count_--;
        tombstone_count_++;

        // Only erase side_map if it still points to this entry. PID-reuse
        // path may have already overwritten side_map[pid] with the new
        // owner's entry — leaving that intact is correct.
        auto it = side_map_.find(entry.pid);
        if (it != side_map_.end() && it->second.index == idx) {
            side_map_.erase(it);
        }
    }

    // Compact: remove tombstones, rebuild indices.
    // After compaction, side_map and LC-RS links are rebuilt from scratch
    // using PSN as the disambiguating tag.
    void compact() {
        std::erase_if(entries_, [](const process_entry& e) { return !e.alive; });

        side_map_.clear();
        for (uint32_t i = 0; i < entries_.size(); i++) {
            side_map_[entries_[i].pid] = side_entry{i, entries_[i].psn};
        }

        rebuild_lc_rs_links();

        alive_count_     = static_cast<uint32_t>(entries_.size());
        tombstone_count_ = 0;
    }

    // Rebuild LC-RS links from (parent_pid, parent_psn) fields.
    // O(N) pass. Used by compact() and when bulk-loaded entries need
    // structural linkage. Parent is matched via side_map[parent_pid].psn
    // == entry.parent_psn — this preserves PID-reuse safety.
    void rebuild_lc_rs_links() {
        for (auto& e : entries_) {
            e.parent_index       = INVALID_IDX;
            e.first_child_index  = INVALID_IDX;
            e.next_sibling_index = INVALID_IDX;
        }

        for (uint32_t i = 0; i < entries_.size(); i++) {
            if (!entries_[i].alive) continue;
            const uint64_t pp = entries_[i].parent_psn;
            if (pp == ROOT_PSN_SENTINEL || pp == INVALID_PSN) continue;

            auto pit = side_map_.find(entries_[i].parent_pid);
            if (pit == side_map_.end()) continue;
            if (pit->second.psn != pp)  continue;       // stale parent_pid -> reused
            uint32_t parent_idx = pit->second.index;
            if (parent_idx == i)           continue;
            if (!entries_[parent_idx].alive) continue;

            entries_[i].parent_index       = parent_idx;
            entries_[i].next_sibling_index = entries_[parent_idx].first_child_index;
            entries_[parent_idx].first_child_index = i;
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

    // ---- Side map access ----

    const std::unordered_map<DWORD, side_entry>& side_map() const { return side_map_; }

private:
    std::vector<process_entry>                entries_;
    std::unordered_map<DWORD, side_entry>     side_map_;   // pid -> {index, psn}
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
        uint32_t ancestor = entries_[dying_idx].parent_index;
        while (ancestor != INVALID_IDX && !entries_[ancestor].alive) {
            ancestor = entries_[ancestor].parent_index;
        }

        uint32_t child = entries_[dying_idx].first_child_index;
        while (child != INVALID_IDX) {
            uint32_t next = entries_[child].next_sibling_index;
            if (entries_[child].alive) {
                entries_[child].parent_index = ancestor;
                entries_[child].parent_pid   = (ancestor != INVALID_IDX)
                    ? entries_[ancestor].pid : 0;
                entries_[child].parent_psn   = (ancestor != INVALID_IDX)
                    ? entries_[ancestor].psn : ROOT_PSN_SENTINEL;
                if (ancestor != INVALID_IDX) {
                    entries_[child].next_sibling_index = entries_[ancestor].first_child_index;
                    entries_[ancestor].first_child_index = child;
                } else {
                    entries_[child].next_sibling_index = INVALID_IDX;
                }
            }
            child = next;
        }
        entries_[dying_idx].first_child_index = INVALID_IDX;
    }
};

} // namespace clew
