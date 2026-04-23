// Clew Component Integration Tests
// =====================================
// Tests core data structures and logic without admin/drivers.
//
// Build (VS2022 dev prompt):
//   cl /EHsc /std:c++23 /DUNICODE /D_UNICODE /I../src /I<vcpkg>/installed/x64-windows/include test_components.cpp /Fe:test_components.exe
//
// Or via CMake (see CMakeLists.txt test target)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "core/log.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <functional>
#include <sstream>

// Components under test
#include "config/types.hpp"
#include "process/flat_tree.hpp"
#include "rules/rule_engine_v3.hpp"
#include "rules/traffic_filter.hpp"
#include "core/port_tracker.hpp"

// ============================================================
// Minimal test framework
// ============================================================

static int g_pass = 0;
static int g_fail = 0;
static std::vector<std::string> g_errors;

#define TEST(name) \
    static void test_##name(); \
    static struct _reg_##name { \
        _reg_##name() { g_tests.push_back({#name, test_##name}); } \
    } _inst_##name; \
    static void test_##name()

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        std::ostringstream ss; \
        ss << __FILE__ << ":" << __LINE__ << ": " << #expr; \
        throw std::runtime_error(ss.str()); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::ostringstream ss; \
        ss << __FILE__ << ":" << __LINE__ << ": " << #a << " != " << #b; \
        throw std::runtime_error(ss.str()); \
    } \
} while(0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

struct TestEntry { std::string name; std::function<void()> fn; };
static std::vector<TestEntry> g_tests;

// ============================================================
// 1. wildcard_match tests
// ============================================================

using clew::wildcard_match;

TEST(wildcard_exact_match) {
    ASSERT_TRUE(wildcard_match("chrome.exe", "chrome.exe"));
    ASSERT_FALSE(wildcard_match("chrome.exe", "firefox.exe"));
}

TEST(wildcard_star) {
    ASSERT_TRUE(wildcard_match("chrome*", "chrome.exe"));
    ASSERT_TRUE(wildcard_match("*chrome*", "google-chrome.exe"));
    ASSERT_TRUE(wildcard_match("*.exe", "test.exe"));
    ASSERT_FALSE(wildcard_match("*.dll", "test.exe"));
}

TEST(wildcard_question) {
    ASSERT_TRUE(wildcard_match("?.exe", "a.exe"));
    ASSERT_FALSE(wildcard_match("?.exe", "ab.exe"));
    ASSERT_TRUE(wildcard_match("test?.exe", "test1.exe"));
}

TEST(wildcard_case_insensitive) {
    ASSERT_TRUE(wildcard_match("Chrome.EXE", "chrome.exe"));
    ASSERT_TRUE(wildcard_match("PYTHON*", "python3.11.exe"));
}

TEST(wildcard_empty) {
    ASSERT_TRUE(wildcard_match("", ""));
    ASSERT_FALSE(wildcard_match("", "something"));
    ASSERT_FALSE(wildcard_match("something", ""));
}

TEST(wildcard_complex) {
    ASSERT_TRUE(wildcard_match("*py*on*", "python.exe"));
    ASSERT_TRUE(wildcard_match("c?r?.exe", "curl.exe"));
    ASSERT_FALSE(wildcard_match("c?r?.exe", "cargo.exe"));
}

// ============================================================
// 2. cmdline_match tests
// ============================================================

using clew::cmdline_match;

TEST(cmdline_keyword_mode) {
    // No wildcards → keyword mode: all fragments must appear as substrings
    ASSERT_TRUE(cmdline_match("udp_client", "C:\\Python\\python.exe udp_client.py --port 8080"));
    ASSERT_TRUE(cmdline_match("udp_client 8080", "C:\\Python\\python.exe udp_client.py --port 8080"));
    ASSERT_FALSE(cmdline_match("udp_client 9090", "C:\\Python\\python.exe udp_client.py --port 8080"));
}

TEST(cmdline_keyword_order_independent) {
    ASSERT_TRUE(cmdline_match("8080 udp_client", "python.exe udp_client.py --port 8080"));
}

TEST(cmdline_glob_mode) {
    // Contains * or ? → glob mode
    ASSERT_TRUE(cmdline_match("*udp_client*", "C:\\Python\\python.exe udp_client.py"));
    ASSERT_FALSE(cmdline_match("*udp_client*8080*", "python.exe udp_client.py --port 9090"));
    ASSERT_TRUE(cmdline_match("*udp_client*8080*", "python.exe udp_client.py --port 8080"));
}

TEST(cmdline_empty_pattern) {
    // Empty pattern should match anything (but cmdline_match is not called with empty)
    // The engine checks emptiness before calling, so test the function directly
    ASSERT_TRUE(cmdline_match("", "anything"));
}

// ============================================================
// 3. flat_tree tests
// ============================================================

using clew::flat_tree;
using clew::raw_process_record;
using clew::INVALID_IDX;
using clew::NO_PROXY;

static flat_tree make_test_tree() {
    flat_tree tree;
    std::vector<raw_process_record> records = {
        {4,    0,    {}, L"System"},
        {100,  4,    {}, L"init.exe"},
        {200,  100,  {}, L"chrome.exe"},
        {201,  200,  {}, L"chrome.exe"},
        {202,  200,  {}, L"chrome.exe"},
        {300,  100,  {}, L"python.exe"},
    };
    tree.build_from_snapshot(records);
    return tree;
}

TEST(tree_build_and_find) {
    auto tree = make_test_tree();
    ASSERT_TRUE(tree.find_by_pid(4) != INVALID_IDX);
    ASSERT_TRUE(tree.find_by_pid(200) != INVALID_IDX);
    ASSERT_TRUE(tree.find_by_pid(999) == INVALID_IDX);
    ASSERT_EQ(tree.alive_count(), 6u);
}

TEST(tree_entry_name) {
    auto tree = make_test_tree();
    uint32_t idx = tree.find_by_pid(200);
    ASSERT_EQ(std::string(tree.at(idx).name_u8), std::string("chrome.exe"));
}

TEST(tree_parent_child) {
    auto tree = make_test_tree();
    uint32_t chrome_idx = tree.find_by_pid(200);
    uint32_t init_idx = tree.find_by_pid(100);
    ASSERT_EQ(tree.at(chrome_idx).parent_pid, (DWORD)100);
    // Chrome (200) should be a child of init (100)
    ASSERT_TRUE(tree.at(chrome_idx).parent_index == init_idx);
}

TEST(tree_add_entry) {
    auto tree = make_test_tree();
    uint32_t old_count = tree.alive_count();
    FILETIME ft = {};
    tree.add_entry(400, 200, ft, L"helper.exe");
    ASSERT_EQ(tree.alive_count(), old_count + 1);
    uint32_t idx = tree.find_by_pid(400);
    ASSERT_TRUE(idx != INVALID_IDX);
    ASSERT_EQ(std::string(tree.at(idx).name_u8), std::string("helper.exe"));
}

TEST(tree_tombstone) {
    auto tree = make_test_tree();
    uint32_t count_before = tree.alive_count();
    FILETIME ft = {};
    tree.tombstone(300, ft);  // Remove python.exe
    ASSERT_EQ(tree.alive_count(), count_before - 1);
    // PID still findable but marked dead
    uint32_t idx = tree.find_by_pid(300);
    ASSERT_TRUE(idx == INVALID_IDX || !tree.at(idx).alive);
}

TEST(tree_visit_descendants) {
    auto tree = make_test_tree();
    uint32_t chrome_idx = tree.find_by_pid(200);
    std::vector<DWORD> descendants;
    tree.visit_descendants(chrome_idx, [&descendants](uint32_t, const auto& entry) {
        descendants.push_back(entry.pid);
    });
    // Chrome 200 has children 201, 202
    ASSERT_EQ(descendants.size(), 2u);
    ASSERT_TRUE(std::find(descendants.begin(), descendants.end(), 201) != descendants.end());
    ASSERT_TRUE(std::find(descendants.begin(), descendants.end(), 202) != descendants.end());
}

TEST(tree_compact) {
    auto tree = make_test_tree();
    FILETIME ft = {};
    // Tombstone several entries
    tree.tombstone(201, ft);
    tree.tombstone(202, ft);
    tree.tombstone(300, ft);
    uint32_t alive_before = tree.alive_count();
    tree.compact();
    ASSERT_EQ(tree.alive_count(), alive_before);
    ASSERT_EQ(tree.tombstone_count(), 0u);
    // Remaining PIDs still findable
    ASSERT_TRUE(tree.find_by_pid(4) != INVALID_IDX);
    ASSERT_TRUE(tree.find_by_pid(200) != INVALID_IDX);
}

// ============================================================
// 4. rule_engine_v3 tests
// ============================================================

using clew::rule_engine_v3;
using clew::AutoRule;

static AutoRule make_rule(std::string_view name, std::string_view process_name,
                          bool hack_tree = false, uint32_t group_id = 1) {
    AutoRule r;
    r.id = std::format("rule_{}", name);
    r.name = name;
    r.enabled = true;
    r.process_name = process_name;
    r.hack_tree = hack_tree;
    r.proxy_group_id = group_id;
    r.protocol = "tcp";
    return r;
}

TEST(rule_auto_match_simple) {
    auto tree = make_test_tree();
    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("chrome_rule", "chrome.exe")});
    engine.apply_auto_rules(tree);

    // All chrome.exe processes should be proxied
    auto hijacked = engine.get_hijacked_pids(tree);
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 200) != hijacked.end());
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 201) != hijacked.end());
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 202) != hijacked.end());
    // python.exe should not be
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 300) == hijacked.end());
}

TEST(rule_auto_match_wildcard) {
    auto tree = make_test_tree();
    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("star_rule", "chr*")});
    engine.apply_auto_rules(tree);

    auto hijacked = engine.get_hijacked_pids(tree);
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 200) != hijacked.end());
}

TEST(rule_hack_tree) {
    auto tree = make_test_tree();
    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("chrome_tree", "chrome.exe", true)});
    engine.apply_auto_rules(tree);

    // hack_tree: chrome.exe matches → root (200) + all descendants (201, 202)
    auto hijacked = engine.get_hijacked_pids(tree);
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 200) != hijacked.end());
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 201) != hijacked.end());
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 202) != hijacked.end());
}

TEST(rule_on_process_start) {
    auto tree = make_test_tree();
    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("py_rule", "python.exe")});
    engine.apply_auto_rules(tree);

    // Simulate new python process starting
    FILETIME ft = {};
    uint32_t idx = tree.add_entry(500, 100, ft, L"python.exe");
    auto match = engine.on_process_start(tree, idx);
    ASSERT_TRUE(match.has_value());
    ASSERT_EQ(match.value(), std::string("rule_py_rule"));
    ASSERT_TRUE(tree.at(idx).is_proxied());
}

TEST(rule_manual_hijack) {
    auto tree = make_test_tree();
    rule_engine_v3 engine;

    engine.manual_hijack(tree, 300, 1);  // hijack python.exe
    ASSERT_TRUE(engine.is_manually_hijacked(tree, 300));

    auto hijacked = engine.get_hijacked_pids(tree);
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 300) != hijacked.end());

    engine.manual_unhijack(tree, 300);
    ASSERT_FALSE(engine.is_manually_hijacked(tree, 300));
}

TEST(rule_exclude_pid) {
    auto tree = make_test_tree();
    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("chrome_rule", "chrome.exe")});

    // Exclude PID 201 before applying
    engine.exclude_pid(tree, "rule_chrome_rule", 201);
    engine.apply_auto_rules(tree);

    auto hijacked = engine.get_hijacked_pids(tree);
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 200) != hijacked.end());
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 201) == hijacked.end());  // excluded
    ASSERT_TRUE(std::find(hijacked.begin(), hijacked.end(), 202) != hijacked.end());
}

TEST(rule_disabled) {
    auto tree = make_test_tree();
    rule_engine_v3 engine;
    auto rule = make_rule("disabled_rule", "chrome.exe");
    rule.enabled = false;
    engine.set_auto_rules({rule});
    engine.apply_auto_rules(tree);

    auto hijacked = engine.get_hijacked_pids(tree);
    ASSERT_TRUE(hijacked.empty());
}

TEST(rule_on_process_exit) {
    auto tree = make_test_tree();
    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("py_rule", "python.exe")});
    engine.apply_auto_rules(tree);

    // Process exit should clean up matched_pids
    engine.on_process_exit(300);
    // Verify rule's internal state is cleaned
    ASSERT_TRUE(engine.auto_rules()[0].matched_pids.find(300) ==
                engine.auto_rules()[0].matched_pids.end());
}

// ============================================================
// 4b. Bug repro: orphan reparent + hack_tree mis-inheritance
// ============================================================

// Bug 1: reparent_children doesn't update parent_pid.
// After compact, rebuild_lc_rs_links uses stale parent_pid → wrong topology.
// Then apply_auto_rules expands descendants of wrong subtree.
TEST(bug_reparent_stale_parent_pid_after_compact) {
    // Tree:
    //   System(4)
    //   ├── spotify.exe(100)
    //   │   └── chrome.exe(101)   ← Spotify's Chromium
    //   └── launcher.exe(200)
    //       └── chrome.exe(300)   ← Google Chrome browser
    flat_tree tree;
    std::vector<raw_process_record> records = {
        {4,    0,    {}, L"System"},
        {100,  4,    {}, L"spotify.exe"},
        {101,  100,  {}, L"chrome.exe"},   // Spotify child
        {200,  4,    {}, L"launcher.exe"},  // Chrome launcher
        {300,  200,  {}, L"chrome.exe"},    // Google Chrome, parent=200
    };
    tree.build_from_snapshot(records);

    // Apply hack_tree rule for spotify.exe → matches PID 100, expands to 101
    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("spotify", "spotify.exe", true, 1)});
    engine.apply_auto_rules(tree);

    // Verify: only spotify subtree is proxied
    ASSERT_TRUE(tree.at(tree.find_by_pid(100)).is_proxied());  // spotify
    ASSERT_TRUE(tree.at(tree.find_by_pid(101)).is_proxied());  // spotify's chrome
    ASSERT_FALSE(tree.at(tree.find_by_pid(300)).is_proxied()); // Google Chrome NOT proxied

    // Now launcher.exe(200) dies → chrome.exe(300) reparented to System(4)
    // parent_index updated to System, but parent_pid stays 200 (stale!)
    FILETIME ft = {};
    tree.tombstone(200, ft);

    // Verify reparent happened (parent_index AND parent_pid updated)
    uint32_t chrome_idx = tree.find_by_pid(300);
    ASSERT_TRUE(chrome_idx != INVALID_IDX);
    // parent_pid should now be 4 (System), not 200 (dead launcher)
    ASSERT_EQ(tree.at(chrome_idx).parent_pid, (DWORD)4);

    // Now PID 200 gets recycled as a Spotify chrome.exe child
    FILETIME ft2 = {1, 0};  // different create_time
    tree.add_entry(200, 100, ft2, L"chrome.exe");  // new chrome.exe under spotify

    // Re-apply rules (simulates config change or periodic rescan)
    engine.apply_auto_rules(tree);

    // New PID 200 (spotify's chrome) should be proxied ← correct
    ASSERT_TRUE(tree.at(tree.find_by_pid(200)).is_proxied());

    // Now trigger compact to force rebuild_lc_rs_links
    // Need enough tombstones: tombstone the old 200 entry already happened,
    // let's add more to trigger threshold
    tree.add_entry(501, 4, ft, L"tmp1.exe");
    tree.add_entry(502, 4, ft, L"tmp2.exe");
    tree.tombstone(501, ft);
    tree.tombstone(502, ft);
    tree.compact();

    // After compact + rebuild_lc_rs_links:
    // chrome.exe(300) has parent_pid=200 (stale, pointing to OLD launcher)
    // But side_map[200] now points to NEW chrome.exe(200) (spotify's child)
    // → rebuild links chrome.exe(300) as child of spotify's chrome.exe(200)!
    // → chrome.exe(300) is now topologically under spotify's subtree

    // Re-apply rules after compact
    engine.apply_auto_rules(tree);

    // FIXED: Google Chrome (PID 300) should NOT be proxied
    // reparent_children now syncs parent_pid, so rebuild_lc_rs_links
    // correctly links PID 300 to System(4), not spotify's chrome.exe(200)
    bool chrome300_proxied = tree.at(tree.find_by_pid(300)).is_proxied();
    ASSERT_FALSE(chrome300_proxied);
}

// Bug 2: on_process_start tree inheritance uses parent_pid (can be stale/recycled)
// A new process whose parent_pid was once in matched_pids gets incorrectly inherited.
TEST(bug_tree_inherit_stale_matched_pid) {
    // Tree:
    //   System(4)
    //   ├── spotify.exe(100)
    //   │   └── chrome.exe(101)
    //   └── explorer.exe(50)
    flat_tree tree;
    std::vector<raw_process_record> records = {
        {4,    0,    {}, L"System"},
        {50,   4,    {}, L"explorer.exe"},
        {100,  4,    {}, L"spotify.exe"},
        {101,  100,  {}, L"chrome.exe"},
    };
    tree.build_from_snapshot(records);

    // Apply hack_tree rule for spotify.exe
    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("spotify", "spotify.exe", true, 1)});
    engine.apply_auto_rules(tree);

    // chrome.exe(101) is in matched_pids (tree-inherited from spotify)
    ASSERT_TRUE(tree.at(tree.find_by_pid(101)).is_proxied());
    ASSERT_TRUE(engine.auto_rules()[0].matched_pids.count(101) > 0);

    // chrome.exe(101) dies
    FILETIME ft = {};
    tree.tombstone(101, ft);
    engine.on_process_exit(101);
    // PID 101 removed from matched_pids
    ASSERT_TRUE(engine.auto_rules()[0].matched_pids.count(101) == 0);

    // PID 101 gets recycled: new unrelated process with parent=explorer
    FILETIME ft2 = {1, 0};
    uint32_t new_idx = tree.add_entry(101, 50, ft2, L"notepad.exe");

    // on_process_start checks tree inheritance:
    // entry.parent_pid = 50 (explorer), not in matched_pids → no tree match
    // This case is actually SAFE (parent_pid is correct for the new process)
    auto match = engine.on_process_start(tree, new_idx);
    ASSERT_FALSE(match.has_value());  // Correct: notepad should not match

    // But what if the timing is different: PID 101 recycled BEFORE on_process_exit?
    // Simulate: re-add chrome.exe(101) as spotify child, put back in matched_pids
    tree.tombstone(101, ft2);
    FILETIME ft3 = {2, 0};
    tree.add_entry(101, 100, ft3, L"chrome.exe");
    engine.apply_auto_rules(tree);
    ASSERT_TRUE(engine.auto_rules()[0].matched_pids.count(101) > 0);

    // Now a NEW process starts claiming parent_pid=101
    // This could be Google Chrome spawning a child whose parent happened to be
    // a recycled PID that's now a Spotify process
    FILETIME ft4 = {3, 0};
    uint32_t child_idx = tree.add_entry(999, 101, ft4, L"gpu-process.exe");
    auto match2 = engine.on_process_start(tree, child_idx);

    // gpu-process.exe(999) gets tree-inherited from chrome.exe(101) which is
    // in spotify's matched_pids. This is CORRECT if 999 is truly a child of 101.
    // The structural issue is that parent_pid=101 might be stale after reparenting,
    // but in this direct case it's actually correct behavior.
    ASSERT_TRUE(match2.has_value());  // Expected: tree-inherited

    // The real danger is Bug 1 (compact+rebuild) which is more deterministic
}

// ============================================================
// 5. PortTracker tests
// ============================================================

using clew::PortTracker;
using clew::TrackerEntry;

TEST(port_tracker_put_get) {
    auto pt = std::make_unique<PortTracker>();
    TrackerEntry e;
    e.remote_addr[0] = 0x0A000001;  // 10.0.0.1
    e.remote_port = 443;
    e.group_id = 1;

    ASSERT_FALSE(pt->is_active(8080));
    pt->put(8080, e);
    ASSERT_TRUE(pt->is_active(8080));
    auto& peek = pt->peek(8080);
    ASSERT_EQ(peek.remote_port, (uint16_t)443);
    ASSERT_EQ(peek.group_id, 1u);
}

TEST(port_tracker_take) {
    auto pt = std::make_unique<PortTracker>();
    TrackerEntry e;
    e.remote_port = 80;
    e.group_id = 2;
    pt->put(9090, e);

    auto taken = pt->take(9090);
    ASSERT_TRUE(taken.has_value());
    ASSERT_EQ(taken->remote_port, (uint16_t)80);
    // take() is non-destructive read (clear is separate)
    ASSERT_TRUE(pt->is_active(9090));
}

TEST(port_tracker_clear) {
    auto pt = std::make_unique<PortTracker>();
    TrackerEntry e;
    e.remote_port = 22;
    pt->put(5000, e);
    ASSERT_TRUE(pt->is_active(5000));
    pt->clear(5000);
    ASSERT_FALSE(pt->is_active(5000));
}

TEST(port_tracker_empty_take) {
    auto pt = std::make_unique<PortTracker>();
    auto result = pt->take(12345);
    ASSERT_FALSE(result.has_value());
}

TEST(port_tracker_overwrite) {
    auto pt = std::make_unique<PortTracker>();
    TrackerEntry e1; e1.group_id = 1;
    TrackerEntry e2; e2.group_id = 2;
    pt->put(7777, e1);
    pt->put(7777, e2);
    ASSERT_EQ(pt->peek(7777).group_id, 2u);
}

// ============================================================
// 6. TrafficFilter tests
// ============================================================

using clew::TrafficFilter;
using clew::TrafficFilterEngine;
using clew::CidrRange;
using clew::PortRange;

TEST(filter_empty_allows_all) {
    TrafficFilter f;
    ASSERT_TRUE(TrafficFilterEngine::should_proxy("8.8.8.8", 443, f));
    ASSERT_TRUE(TrafficFilterEngine::should_proxy("1.2.3.4", 80, f));
}

TEST(filter_exclude_cidr) {
    TrafficFilter f;
    f.exclude_cidrs = {CidrRange::parse("10.0.0.0/8")};
    ASSERT_FALSE(TrafficFilterEngine::should_proxy("10.0.0.1", 443, f));
    ASSERT_FALSE(TrafficFilterEngine::should_proxy("10.255.255.255", 80, f));
    ASSERT_TRUE(TrafficFilterEngine::should_proxy("11.0.0.1", 443, f));
}

TEST(filter_include_ports) {
    TrafficFilter f;
    f.include_ports = {PortRange::parse("443"), PortRange::parse("80")};
    ASSERT_TRUE(TrafficFilterEngine::should_proxy("8.8.8.8", 443, f));
    ASSERT_TRUE(TrafficFilterEngine::should_proxy("8.8.8.8", 80, f));
    ASSERT_FALSE(TrafficFilterEngine::should_proxy("8.8.8.8", 22, f));
}

TEST(filter_exclude_port) {
    TrafficFilter f;
    f.exclude_ports = {PortRange::parse("22")};
    ASSERT_FALSE(TrafficFilterEngine::should_proxy("8.8.8.8", 22, f));
    ASSERT_TRUE(TrafficFilterEngine::should_proxy("8.8.8.8", 443, f));
}

// ============================================================
// 7. JSON round-trip tests
// ============================================================

using nlohmann::json;

TEST(json_autorule_roundtrip) {
    AutoRule r;
    r.id = "test_id";
    r.name = "Test Rule";
    r.enabled = true;
    r.process_name = "curl*";
    r.cmdline_pattern = "download";
    r.image_path_pattern = "C:\\tools\\";
    r.hack_tree = true;
    r.proxy_group_id = 2;
    r.protocol = "both";

    json j = r;
    AutoRule r2 = j.get<AutoRule>();

    ASSERT_EQ(r2.id, r.id);
    ASSERT_EQ(r2.name, r.name);
    ASSERT_EQ(r2.enabled, r.enabled);
    ASSERT_EQ(r2.process_name, r.process_name);
    ASSERT_EQ(r2.cmdline_pattern, r.cmdline_pattern);
    ASSERT_EQ(r2.image_path_pattern, r.image_path_pattern);
    ASSERT_EQ(r2.hack_tree, r.hack_tree);
    ASSERT_EQ(r2.proxy_group_id, r.proxy_group_id);
    ASSERT_EQ(r2.protocol, r.protocol);
}

TEST(json_autorule_defaults) {
    // Deserialize from minimal JSON — should use defaults
    json j = {{"id", "x"}, {"name", "y"}};
    AutoRule r = j.get<AutoRule>();
    ASSERT_EQ(r.enabled, true);
    ASSERT_EQ(r.hack_tree, true);  // default in from_json
    ASSERT_EQ(r.protocol, std::string("tcp"));
    ASSERT_TRUE(r.image_path_pattern.empty());
}

// ============================================================
// Main runner
// ============================================================

int main() {
    // Initialize quill for test logging
    quill::Backend::start();
    auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("test_console");
    clew::g_logger = quill::Frontend::create_or_get_logger("test", std::move(sink));
    clew::g_logger->set_log_level(quill::LogLevel::Info);

    std::cout << "============================================================\n";
    std::cout << "Clew Component Tests\n";
    std::cout << "============================================================\n\n";

    for (auto& [name, fn] : g_tests) {
        try {
            fn();
            g_pass++;
            std::cout << "  [PASS] " << name << "\n";
        } catch (const std::exception& e) {
            g_fail++;
            g_errors.push_back(name + ": " + e.what());
            std::cout << "  [FAIL] " << name << ": " << e.what() << "\n";
        }
    }

    std::cout << "\n============================================================\n";
    std::cout << "Results: " << g_pass << " passed, " << g_fail << " failed, "
              << (g_pass + g_fail) << " total\n";
    if (!g_errors.empty()) {
        std::cout << "\nFailures:\n";
        for (auto& e : g_errors) std::cout << "  - " << e << "\n";
    }
    std::cout << "============================================================\n";

    return g_fail == 0 ? 0 : 1;
}
