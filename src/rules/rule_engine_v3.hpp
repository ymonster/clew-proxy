#pragma once

// Rule Engine v3: operates directly on flat_tree entries.
// All functions run on the strand — no internal mutex.
//
// Priority: Manual hijack > Auto rules (config order) > DIRECT
// hack_tree: LC-RS descendant traversal (O(subtree) vs v1's iterative O(N))
//
// SOCKET handler reads entry.group_id directly (hot path, O(1)).
// Rule engine sets group_id/flags when processes start or rules change.

#include <string>
#include <format>
#include <vector>
#include <unordered_set>
#include <optional>
#include <algorithm>
#include "core/log.hpp"

#include "process/flat_tree.hpp"
#include "config/types.hpp"
#include "rules/traffic_filter.hpp"

namespace clew {

// ============================================================
// Wildcard matching (from v1 rule_engine, case-insensitive)
// ============================================================

inline bool wildcard_match(const std::string& pattern, const std::string& text,
                           bool case_insensitive = true)
{
    auto to_lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };

    size_t pi = 0;
    size_t ti = 0;
    size_t star_pi = std::string::npos;
    size_t star_ti = 0;

    while (ti < text.size()) {
        if (pi < pattern.size()) {
            char pc = case_insensitive ? to_lower(pattern[pi]) : pattern[pi];
            char tc = case_insensitive ? to_lower(text[ti]) : text[ti];

            if (pc == '?') { pi++; ti++; continue; }
            if (pc == '*') { star_pi = pi++; star_ti = ti; continue; }
            if (pc == tc)  { pi++; ti++; continue; }
        }
        if (star_pi != std::string::npos) {
            pi = star_pi + 1;
            ti = ++star_ti;
            continue;
        }
        return false;
    }
    while (pi < pattern.size() && pattern[pi] == '*') pi++;
    return pi == pattern.size();
}

// ============================================================
// Cmdline matching: auto-selects keyword vs glob mode
//   - Keyword mode (no * or ?): split by space, all fragments must be
//     case-insensitive substrings of cmdline, order-independent
//   - Glob mode (has * or ?): full wildcard match, order-sensitive
// ============================================================

inline bool cmdline_match(const std::string& pattern, const std::string& cmdline) {
    // Detect mode: glob if pattern contains * or ?
    if (pattern.find_first_of("*?") != std::string::npos) {
        return wildcard_match(pattern, cmdline);
    }

    // Keyword mode: all space-separated fragments must be substrings (case-insensitive)
    auto to_lower_str = [](const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    };

    std::string lower_cmdline = to_lower_str(cmdline);
    std::string lower_pattern = to_lower_str(pattern);

    size_t pos = 0;
    while (pos < lower_pattern.size()) {
        // Skip spaces
        while (pos < lower_pattern.size() && lower_pattern[pos] == ' ') pos++;
        if (pos >= lower_pattern.size()) break;

        // Extract fragment
        size_t end = lower_pattern.find(' ', pos);
        if (end == std::string::npos) end = lower_pattern.size();
        std::string fragment = lower_pattern.substr(pos, end - pos);
        pos = end;

        // Each fragment must appear somewhere in cmdline
        if (lower_cmdline.find(fragment) == std::string::npos) return false;
    }
    return true;
}

// ============================================================
// Rule match result (for API / tracker)
// ============================================================

struct RuleMatchResult {
    std::string rule_id;
    std::string rule_type;  // "manual" | "auto" | "tree-inherited"
    uint32_t group_id{0};
};

// ============================================================
// Rule Engine v3
// ============================================================

class rule_engine_v3 {
public:
    rule_engine_v3() = default;

    // Load auto rules from config (called on strand)
    void set_auto_rules(const std::vector<AutoRule>& rules) {
        auto_rules_ = rules;
        // Clear runtime state
        for (auto& r : auto_rules_) {
            r.matched_pids.clear();
        }
        refresh_cmdline_flag();
    }

    std::vector<AutoRule>& auto_rules() { return auto_rules_; }
    const std::vector<AutoRule>& auto_rules() const { return auto_rules_; }

    // ---- Manual hijack (strand-safe) ----

    void manual_hijack(flat_tree& tree, DWORD pid, uint32_t group_id) {
        uint32_t idx = tree.find_by_pid(pid);
        if (idx == INVALID_IDX) return;

        auto& entry = tree.at(idx);
        entry.group_id = group_id;
        entry.set_flag(entry_flags::MANUAL_HIJACK);

        PC_LOG_INFO("[RULE] Manual hijack: PID={} group={}", pid, group_id);
    }

    void manual_unhijack(flat_tree& tree, DWORD pid) {
        uint32_t idx = tree.find_by_pid(pid);
        if (idx == INVALID_IDX) return;

        auto& entry = tree.at(idx);
        entry.clear_flag(entry_flags::MANUAL_HIJACK);

        // If not auto-matched either, clear group_id
        if (!entry.has_flag(entry_flags::AUTO_MATCHED)) {
            entry.group_id = NO_PROXY;
        }

        PC_LOG_INFO("[RULE] Manual unhijack: PID={}", pid);
    }

    // Manual hijack with tree expansion: sets MANUAL_HIJACK on target + all descendants
    void manual_hijack_tree(flat_tree& tree, DWORD pid, uint32_t group_id) {
        uint32_t idx = tree.find_by_pid(pid);
        if (idx == INVALID_IDX) return;

        tree.at(idx).group_id = group_id;
        tree.at(idx).set_flag(entry_flags::MANUAL_HIJACK);

        tree.visit_descendants(idx, [&](uint32_t ci, const process_entry&) {
            auto& c = tree.at(ci);
            c.group_id = group_id;
            c.set_flag(entry_flags::MANUAL_HIJACK);
        });

        PC_LOG_INFO("[RULE] Manual hijack tree: PID={} group={}", pid, group_id);
    }

    // Manual unhijack with cascade: clears MANUAL_HIJACK on target + all descendants
    void manual_unhijack_tree(flat_tree& tree, DWORD pid) {
        uint32_t idx = tree.find_by_pid(pid);
        if (idx == INVALID_IDX) return;

        auto& entry = tree.at(idx);
        entry.clear_flag(entry_flags::MANUAL_HIJACK);
        if (!entry.has_flag(entry_flags::AUTO_MATCHED)) entry.group_id = NO_PROXY;

        tree.visit_descendants(idx, [&](uint32_t ci, const process_entry& child) {
            if (!child.has_flag(entry_flags::MANUAL_HIJACK)) return;
            auto& c = tree.at(ci);
            c.clear_flag(entry_flags::MANUAL_HIJACK);
            if (!c.has_flag(entry_flags::AUTO_MATCHED)) c.group_id = NO_PROXY;
        });

        PC_LOG_INFO("[RULE] Manual unhijack tree: PID={}", pid);
    }

    bool is_manually_hijacked(const flat_tree& tree, DWORD pid) const {
        uint32_t idx = tree.find_by_pid(pid);
        if (idx == INVALID_IDX) return false;
        return tree.at(idx).has_flag(entry_flags::MANUAL_HIJACK);
    }

    // ---- Auto rule matching (strand-safe) ----

    // Full scan: apply all auto rules to entire tree.
    // Called on rule changes or initial setup.
    void apply_auto_rules(flat_tree& tree) {
        refresh_cmdline_flag();
        // Clear all auto-match state first
        for (auto& entry : tree.entries()) {
            if (!entry.alive) continue;
            if (entry.has_flag(entry_flags::AUTO_MATCHED)) {
                entry.clear_flag(entry_flags::AUTO_MATCHED);
                if (!entry.has_flag(entry_flags::MANUAL_HIJACK)) {
                    entry.group_id = NO_PROXY;
                }
            }
        }
        for (auto& rule : auto_rules_) {
            rule.matched_pids.clear();
        }

        // Apply each rule
        for (auto& rule : auto_rules_) {
            if (!rule.enabled) continue;
            apply_single_rule(tree, rule);
        }
    }

    // Check a newly started process against auto rules.
    // Called from ETW ProcessStart handler.
    std::optional<std::string> on_process_start(flat_tree& tree, uint32_t idx) {
        const auto& entry = tree.at(idx);
        if (!entry.alive) return std::nullopt;

        std::string name(entry.name_u8);

        for (auto& rule : auto_rules_) {
            if (!rule.enabled) continue;
            if (rule.excluded_pids.contains(entry.pid)) continue;
            if (rule.matched_pids.contains(entry.pid)) continue;

            bool name_match = false;
            bool tree_match = false;

            // Direct name match
            if (!rule.process_name.empty() && wildcard_match(rule.process_name, name)) {
                if (!rule.cmdline_pattern.empty() && !check_cmdline(tree, idx, rule)) {
                    continue;  // name matched but cmdline didn't
                }
                if (!rule.image_path_pattern.empty() && !check_image_path(tree, idx, rule)) {
                    continue;  // name matched but image path didn't
                }
                name_match = true;
            }

            // Tree inheritance: parent is already matched by this rule
            if (!name_match && rule.hack_tree && entry.parent_pid != 0) {
                if (rule.matched_pids.contains(entry.parent_pid)) {
                    tree_match = true;
                }
            }

            if (name_match || tree_match) {
                rule.matched_pids.insert(entry.pid);
                mark_proxied(tree, idx, rule);

                // For hack_tree name matches: also expand to all existing descendants
                if (name_match && rule.hack_tree) {
                    expand_descendants(tree, idx, rule);
                }

                PC_LOG_INFO("[RULE] {} PID={} ({}) → rule '{}' group={}",
                             tree_match ? "Tree-inherited" : "Auto-matched",
                             entry.pid, name, rule.name, rule.proxy_group_id);
                return rule.id;
            }
        }

        // Unified tree inheritance: if no auto rule matched,
        // check if parent is manually hijacked → inherit
        if (entry.parent_pid != 0) {
            uint32_t parent_idx = tree.find_by_pid(entry.parent_pid);
            if (parent_idx != INVALID_IDX) {
                const auto& parent = tree.at(parent_idx);
                if (parent.alive && parent.has_flag(entry_flags::MANUAL_HIJACK)) {
                    auto& child = tree.at(idx);
                    child.group_id = parent.group_id;
                    child.set_flag(entry_flags::MANUAL_HIJACK);

                    PC_LOG_INFO("[RULE] Manual tree-inherit: PID={} ({}) from parent PID={} group={}",
                                 entry.pid, name, entry.parent_pid, parent.group_id);
                    return std::format("manual:tree:{}", entry.parent_pid);
                }
            }
        }

        return std::nullopt;
    }

    // Process exit: cleanup from all rule state
    void on_process_exit(DWORD pid) {
        for (auto& rule : auto_rules_) {
            rule.matched_pids.erase(pid);
            rule.excluded_pids.erase(pid);
        }
    }

    // Exclude/unexclude a PID from a specific rule
    bool exclude_pid(flat_tree& tree, const std::string& rule_id, DWORD pid) {
        for (auto& rule : auto_rules_) {
            if (rule.id != rule_id) continue;
            rule.excluded_pids.insert(pid);
            rule.matched_pids.erase(pid);

            uint32_t idx = tree.find_by_pid(pid);
            if (idx != INVALID_IDX) {
                auto& entry = tree.at(idx);
                entry.clear_flag(entry_flags::AUTO_MATCHED);
                if (!entry.has_flag(entry_flags::MANUAL_HIJACK))
                    entry.group_id = NO_PROXY;
            }
            return true;
        }
        return false;
    }

    bool unexclude_pid(const std::string& rule_id, DWORD pid) {
        for (auto& rule : auto_rules_) {
            if (rule.id != rule_id) continue;
            rule.excluded_pids.erase(pid);
            return true;
        }
        return false;
    }

    // Get hijacked PIDs (for API)
    std::vector<DWORD> get_hijacked_pids(const flat_tree& tree) const {
        std::vector<DWORD> result;
        for (const auto& entry : tree.entries()) {
            if (entry.alive && entry.is_proxied()) {
                result.push_back(entry.pid);
            }
        }
        return result;
    }

    // Find which rule matched a PID (for API display)
    // Check if a PID should be proxied for a specific protocol ("tcp" or "udp").
    // Manual hijack always matches both protocols.
    bool should_proxy_protocol(const flat_tree& tree, DWORD pid, const std::string& proto) const {
        uint32_t idx = tree.find_by_pid(pid);
        if (idx == INVALID_IDX) return false;
        const auto& entry = tree.at(idx);
        if (!entry.alive || !entry.is_proxied()) return false;

        // Manual hijack: always proxy both TCP and UDP
        if (entry.has_flag(entry_flags::MANUAL_HIJACK)) return true;

        // Auto rule: check protocol field
        for (const auto& rule : auto_rules_) {
            if (rule.matched_pids.contains(pid)) {
                if (proto == "tcp") return rule.matches_tcp();
                if (proto == "udp") return rule.matches_udp();
                return true;
            }
        }
        return false;
    }

    std::optional<RuleMatchResult> get_match_info(const flat_tree& tree, DWORD pid) const {
        uint32_t idx = tree.find_by_pid(pid);
        if (idx == INVALID_IDX) return std::nullopt;

        const auto& entry = tree.at(idx);
        if (!entry.is_proxied()) return std::nullopt;

        RuleMatchResult result;
        result.group_id = entry.group_id;

        if (entry.has_flag(entry_flags::MANUAL_HIJACK)) {
            result.rule_type = "manual";
            result.rule_id = std::format("manual:{}", pid);
            return result;
        }

        // Find which auto rule matched this PID
        for (const auto& rule : auto_rules_) {
            if (rule.matched_pids.contains(pid)) {
                result.rule_type = "auto";
                result.rule_id = rule.id;
                return result;
            }
        }

        return std::nullopt;
    }

private:
    std::vector<AutoRule> auto_rules_;
    bool any_rule_uses_cmdline_ = false;

    void refresh_cmdline_flag() {
        any_rule_uses_cmdline_ = std::any_of(auto_rules_.begin(), auto_rules_.end(),
            [](const AutoRule& r) { return !r.cmdline_pattern.empty(); });
    }

    // Lazy cmdline check: returns true if cmdline matches (or rule has no cmdline_pattern).
    // Queries OS on first call per process, caches result in entry.cmdline_cache.
    // Sentinel "\x01" means "queried but empty/failed" — avoids repeated syscalls.
    bool check_cmdline(flat_tree& tree, uint32_t idx, const AutoRule& rule) {
        if (rule.cmdline_pattern.empty()) return true;

        auto& entry = tree.at(idx);
        if (entry.cmdline_cache.empty()) {
            entry.cmdline_cache = query_process_cmdline(entry.pid);
            if (entry.cmdline_cache.empty()) {
                entry.cmdline_cache = "\x01";  // sentinel: queried but failed/empty
            }
        }
        return entry.cmdline_cache[0] != '\x01' &&
               cmdline_match(rule.cmdline_pattern, entry.cmdline_cache);
    }

    // Lazy image_path check: prefix match (case-insensitive).
    // Only queries OS if rule has image_path_pattern and name already matched.
    bool check_image_path(flat_tree& tree, uint32_t idx, const AutoRule& rule) {
        if (rule.image_path_pattern.empty()) return true;

        auto& entry = tree.at(idx);
        if (entry.image_path_cache.empty()) {
            entry.image_path_cache = query_process_image_path(entry.pid);
            if (entry.image_path_cache.empty()) {
                entry.image_path_cache = "\x01";
            }
        }
        if (entry.image_path_cache[0] == '\x01') return false;

        const auto& path = entry.image_path_cache;
        const auto& pattern = rule.image_path_pattern;
        if (path.size() < pattern.size()) return false;
        return std::equal(pattern.begin(), pattern.end(), path.begin(),
            [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            });
    }

    // Apply a single auto rule to all alive entries
    void apply_single_rule(flat_tree& tree, AutoRule& rule) {
        // Single scan: collect matches via guard clauses. For hack_tree mode
        // we stage them in `name_matched` first; otherwise insert directly.
        std::unordered_set<DWORD> name_matched;

        for (uint32_t i = 0; i < tree.entries().size(); i++) {
            const auto& entry = tree.entries()[i];

            if (!entry.alive || rule.excluded_pids.contains(entry.pid)) continue;

            if (!rule.process_name.empty() && !wildcard_match(rule.process_name, entry.name_u8)) continue;
            if (!rule.cmdline_pattern.empty() && !check_cmdline(tree, i, rule)) continue;
            if (!rule.image_path_pattern.empty() && !check_image_path(tree, i, rule)) continue;

            if (rule.hack_tree) {
                name_matched.insert(entry.pid);
            } else {
                rule.matched_pids.insert(entry.pid);
                mark_proxied(tree, i, rule);
            }
        }

        // hack_tree mode: find roots (parent not matched) and expand descendants.
        if (rule.hack_tree) {
            for (DWORD pid : name_matched) {
                uint32_t idx = tree.find_by_pid(pid);
                if (idx == INVALID_IDX) continue;
                const auto& entry = tree.at(idx);

                rule.matched_pids.insert(pid);
                mark_proxied(tree, idx, rule);

                if (!name_matched.contains(entry.parent_pid)) {
                    expand_descendants(tree, idx, rule);
                }
            }
        }

        if (!rule.matched_pids.empty()) {
            PC_LOG_INFO("[RULE] '{}': {} processes matched", rule.name, rule.matched_pids.size());
        }
    }

    // Expand all descendants of an entry via LC-RS traversal
    void expand_descendants(flat_tree& tree, uint32_t idx, AutoRule& rule) {
        tree.visit_descendants(idx, [&](uint32_t child_idx, const process_entry& child) {
            if (rule.excluded_pids.contains(child.pid)) return;
            if (rule.matched_pids.contains(child.pid)) return;
            rule.matched_pids.insert(child.pid);
            mark_proxied(tree, child_idx, rule);
        });
    }

    // Set group_id and AUTO_MATCHED flag on an entry
    void mark_proxied(flat_tree& tree, uint32_t idx, const AutoRule& rule) {
        auto& entry = tree.at(idx);
        // Don't overwrite manual hijack
        if (entry.has_flag(entry_flags::MANUAL_HIJACK)) return;
        entry.group_id = rule.proxy_group_id;
        entry.set_flag(entry_flags::AUTO_MATCHED);
    }
};

} // namespace clew
