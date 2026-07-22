// Clew Component Integration Tests
// =====================================
// Tests core data structures and logic without admin/drivers.
//
// Build (VS2022 dev prompt):
//   cl /EHsc /std:c++latest /utf-8 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /DNOMINMAX ^
//      /I../src /I"%VCPKG_ROOT%/installed/x64-windows/include" ^
//      test_components.cpp ^
//      /link /LIBPATH:"%VCPKG_ROOT%/installed/x64-windows/lib" ws2_32.lib
//
// /std:c++23 isn't recognized by VS2022 14.44 — use /std:c++latest.
// /DNOMINMAX — quill/std::numeric_limits collides with windows.h max() macro.
// /utf-8 — file contains UTF-8 (CN comments); cp 936 default mis-parses.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <cassert>
#include <functional>
#include <future>
#include <sstream>
#include <thread>

#include "core/log.hpp"

// Components under test
#include "config/types.hpp"
#include "process/flat_tree.hpp"
#include "rules/rule_engine_v3.hpp"
#include "rules/traffic_filter.hpp"
#include "core/port_tracker.hpp"
#include "udp/udp_port_tracker.hpp"
#include "udp/socks5_udp_session.hpp"

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

// Minimal local SOCKS5 UDP ASSOCIATE peer. It performs one handshake and
// captures one UDP datagram, keeping the component test independent of any
// external proxy or network service.
struct FakeSocks5UdpPeer {
    explicit FakeSocks5UdpPeer(bool wildcard_reply)
        : worker([this, wildcard_reply] { run(wildcard_reply); }) {}

    ~FakeSocks5UdpPeer() {
        if (worker.joinable()) worker.join();
    }

    uint16_t proxy_port() { return proxy_port_future.get(); }
    std::vector<uint8_t> received() { return received_future.get(); }

private:
    std::promise<uint16_t> proxy_port_promise;
    std::future<uint16_t> proxy_port_future = proxy_port_promise.get_future();
    std::promise<std::vector<uint8_t>> received_promise;
    std::future<std::vector<uint8_t>> received_future = received_promise.get_future();
    std::thread worker;

    void run(bool wildcard_reply) {
        try {
            asio::io_context ioc;
            asio::ip::tcp::acceptor acceptor(
                ioc, {asio::ip::address_v4::loopback(), 0});
            asio::ip::udp::socket relay(
                ioc, {asio::ip::address_v4::loopback(), 0});
            proxy_port_promise.set_value(acceptor.local_endpoint().port());

            asio::ip::tcp::socket control(ioc);
            acceptor.accept(control);

            std::array<uint8_t, 3> auth{};
            asio::read(control, asio::buffer(auth));
            const std::array<uint8_t, 2> auth_reply{0x05, 0x00};
            asio::write(control, asio::buffer(auth_reply));

            std::array<uint8_t, 10> associate{};
            asio::read(control, asio::buffer(associate));

            const auto relay_port = relay.local_endpoint().port();
            const auto reply_addr = wildcard_reply
                ? asio::ip::address_v4::any().to_bytes()
                : asio::ip::address_v4::loopback().to_bytes();
            std::array<uint8_t, 10> reply{
                0x05, 0x00, 0x00, 0x01,
                reply_addr[0], reply_addr[1], reply_addr[2], reply_addr[3],
                static_cast<uint8_t>(relay_port >> 8),
                static_cast<uint8_t>(relay_port & 0xff)};
            asio::write(control, asio::buffer(reply));

            std::array<uint8_t, 512> data{};
            asio::ip::udp::endpoint sender;
            auto n = relay.receive_from(asio::buffer(data), sender);
            received_promise.set_value(
                std::vector<uint8_t>(data.begin(), data.begin() + n));
        } catch (...) {
            try { proxy_port_promise.set_exception(std::current_exception()); }
            catch (...) {}
            try { received_promise.set_exception(std::current_exception()); }
            catch (...) {}
        }
    }
};

static bool run_udp_send(asio::io_context& ioc,
                         const std::shared_ptr<clew::Socks5UdpSession>& session,
                         std::vector<uint8_t> frame) {
    auto result = asio::co_spawn(
        ioc, session->async_send_udp(std::move(frame)), asio::use_future);
    std::thread io_thread([&ioc] { ioc.run(); });
    bool sent = result.get();
    session->close();
    ioc.stop();
    io_thread.join();
    return sent;
}

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
using clew::INVALID_IDX;
using clew::NO_PROXY;
using clew::ROOT_PSN_SENTINEL;

// Test helpers — translate legacy add/tombstone signatures (pre-PSN refactor)
// onto the new PSN-keyed interface. PSNs are assigned monotonically per
// add_test_entry invocation; parent_psn is looked up from side_map.
static uint64_t g_next_test_psn = 1;

static uint32_t add_test_entry(flat_tree& t, DWORD pid, DWORD parent_pid,
                               FILETIME ft, const wchar_t* name) {
    uint64_t my_psn = g_next_test_psn++;
    uint64_t parent_psn = ROOT_PSN_SENTINEL;
    if (parent_pid != 0) {
        auto& sm = t.side_map();
        if (auto it = sm.find(parent_pid); it != sm.end()) {
            parent_psn = it->second.psn;
        }
    }
    uint32_t idx = t.add_entry(pid, parent_pid, my_psn, parent_psn, ft, name);
    if (parent_psn == ROOT_PSN_SENTINEL || parent_pid == 0) {
        t.mark_root(idx);
    } else {
        uint32_t pidx = t.find_by_pid_psn(parent_pid, parent_psn);
        if (pidx != INVALID_IDX) {
            t.attach_child(pidx, idx);
        } else {
            t.mark_root(idx);
        }
    }
    return idx;
}

static bool tombstone_by_pid(flat_tree& t, DWORD pid) {
    auto& sm = t.side_map();
    auto it = sm.find(pid);
    if (it == sm.end()) return false;
    return t.tombstone(pid, it->second.psn);
}

static flat_tree make_test_tree() {
    flat_tree tree;
    FILETIME ft{};
    add_test_entry(tree, 4,   0,   ft, L"System");
    add_test_entry(tree, 100, 4,   ft, L"init.exe");
    add_test_entry(tree, 200, 100, ft, L"chrome.exe");
    add_test_entry(tree, 201, 200, ft, L"chrome.exe");
    add_test_entry(tree, 202, 200, ft, L"chrome.exe");
    add_test_entry(tree, 300, 100, ft, L"python.exe");
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
    add_test_entry(tree, 400, 200, ft, L"helper.exe");
    ASSERT_EQ(tree.alive_count(), old_count + 1);
    uint32_t idx = tree.find_by_pid(400);
    ASSERT_TRUE(idx != INVALID_IDX);
    ASSERT_EQ(std::string(tree.at(idx).name_u8), std::string("helper.exe"));
}

TEST(tree_tombstone) {
    auto tree = make_test_tree();
    uint32_t count_before = tree.alive_count();
    tombstone_by_pid(tree, 300);  // Remove python.exe
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
    // Tombstone several entries
    tombstone_by_pid(tree, 201);
    tombstone_by_pid(tree, 202);
    tombstone_by_pid(tree, 300);
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
    uint32_t idx = add_test_entry(tree, 500, 100, ft, L"python.exe");
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
    // Post-PSN refactor: parent_psn (NOT parent_pid) is the disambiguating
    // tag during rebuild_lc_rs_links. PID-reuse no longer corrupts topology
    // because the new PID owner has a fresh PSN that doesn't match the
    // dead launcher's PSN.
    //
    // Tree:
    //   System(4)
    //   ├── spotify.exe(100)
    //   │   └── chrome.exe(101)   ← Spotify's Chromium
    //   └── launcher.exe(200)
    //       └── chrome.exe(300)   ← Google Chrome browser
    flat_tree tree;
    FILETIME ft = {};
    add_test_entry(tree, 4,   0,   ft, L"System");
    add_test_entry(tree, 100, 4,   ft, L"spotify.exe");
    add_test_entry(tree, 101, 100, ft, L"chrome.exe");    // Spotify child
    add_test_entry(tree, 200, 4,   ft, L"launcher.exe");  // Chrome launcher
    add_test_entry(tree, 300, 200, ft, L"chrome.exe");    // Google Chrome, parent=200

    // Apply hack_tree rule for spotify.exe → matches PID 100, expands to 101
    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("spotify", "spotify.exe", true, 1)});
    engine.apply_auto_rules(tree);

    // Verify: only spotify subtree is proxied
    ASSERT_TRUE(tree.at(tree.find_by_pid(100)).is_proxied());  // spotify
    ASSERT_TRUE(tree.at(tree.find_by_pid(101)).is_proxied());  // spotify's chrome
    ASSERT_FALSE(tree.at(tree.find_by_pid(300)).is_proxied()); // Google Chrome NOT proxied

    // launcher.exe(200) dies → chrome.exe(300) reparented to System(4)
    tombstone_by_pid(tree, 200);

    uint32_t chrome_idx = tree.find_by_pid(300);
    ASSERT_TRUE(chrome_idx != INVALID_IDX);
    ASSERT_EQ(tree.at(chrome_idx).parent_pid, (DWORD)4);

    // Now PID 200 gets recycled as a Spotify chrome.exe child.
    // The new entry gets a fresh PSN (post-launcher), so even though
    // chrome.exe(300) still has parent_pid=200 from rebuild's view, the
    // parent_psn field on chrome(300) points to the now-dead launcher's
    // PSN and won't match the new entry.
    add_test_entry(tree, 200, 100, ft, L"chrome.exe");
    engine.apply_auto_rules(tree);
    ASSERT_TRUE(tree.at(tree.find_by_pid(200)).is_proxied());

    // Trigger compact (need enough tombstones for the 20% threshold).
    add_test_entry(tree, 501, 4, ft, L"tmp1.exe");
    add_test_entry(tree, 502, 4, ft, L"tmp2.exe");
    tombstone_by_pid(tree, 501);
    tombstone_by_pid(tree, 502);
    tree.compact();

    // After compact + rebuild_lc_rs_links: chrome.exe(300) was reparented
    // to System(4) by reparent_children — its parent_psn now points to
    // System's PSN. The new chrome.exe(200) has an unrelated PSN.
    // rebuild matches by parent_psn, so chrome(300) correctly lands under
    // System.
    engine.apply_auto_rules(tree);
    ASSERT_FALSE(tree.at(tree.find_by_pid(300)).is_proxied());
}

// Bug 2: on_process_start tree inheritance uses parent_pid (can be stale/recycled)
// A new process whose parent_pid was once in matched_pids gets incorrectly inherited.
TEST(bug_tree_inherit_stale_matched_pid) {
    // Same scenario as before — the rule engine still uses parent_pid for
    // tree inheritance lookups. The PSN refactor doesn't fix this codepath
    // (the matched_pids set is keyed by PID), so behavior unchanged here.
    //
    // Tree:
    //   System(4)
    //   ├── spotify.exe(100)
    //   │   └── chrome.exe(101)
    //   └── explorer.exe(50)
    flat_tree tree;
    FILETIME ft = {};
    add_test_entry(tree, 4,   0,   ft, L"System");
    add_test_entry(tree, 50,  4,   ft, L"explorer.exe");
    add_test_entry(tree, 100, 4,   ft, L"spotify.exe");
    add_test_entry(tree, 101, 100, ft, L"chrome.exe");

    rule_engine_v3 engine;
    engine.set_auto_rules({make_rule("spotify", "spotify.exe", true, 1)});
    engine.apply_auto_rules(tree);

    ASSERT_TRUE(tree.at(tree.find_by_pid(101)).is_proxied());
    ASSERT_TRUE(engine.auto_rules()[0].matched_pids.count(101) > 0);

    tombstone_by_pid(tree, 101);
    engine.on_process_exit(101);
    ASSERT_TRUE(engine.auto_rules()[0].matched_pids.count(101) == 0);

    // PID 101 recycled with explorer parent — should NOT match spotify rule.
    uint32_t new_idx = add_test_entry(tree, 101, 50, ft, L"notepad.exe");
    auto match = engine.on_process_start(tree, new_idx);
    ASSERT_FALSE(match.has_value());

    // Edge case: re-add chrome(101) under spotify, PID 999 child claims 101.
    tombstone_by_pid(tree, 101);
    add_test_entry(tree, 101, 100, ft, L"chrome.exe");
    engine.apply_auto_rules(tree);
    ASSERT_TRUE(engine.auto_rules()[0].matched_pids.count(101) > 0);

    uint32_t child_idx = add_test_entry(tree, 999, 101, ft, L"gpu-process.exe");
    auto match2 = engine.on_process_start(tree, child_idx);
    ASSERT_TRUE(match2.has_value());  // Tree-inherited from chrome(101)
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
using clew::IpExcludePolicy;
using clew::IpExcludeReason;
using clew::UdpTrackerEntry;
using clew::UdpPortTracker;
using clew::udp_ip_exclude_reason;

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

TEST(ip_exclude_policy_global_precedes_rule) {
    IpExcludePolicy policy;
    policy.global_cidrs = {CidrRange::parse("192.0.2.0/24")};
    policy.rule_cidrs = {CidrRange::parse("192.0.2.0/25")};
    ASSERT_EQ(policy.evaluate(CidrRange::ip_to_uint("192.0.2.13")),
              IpExcludeReason::global);
    ASSERT_EQ(policy.evaluate(CidrRange::ip_to_uint("198.51.100.1")),
              IpExcludeReason::none);
}

TEST(ip_exclude_policy_empty_excludes_nothing) {
    IpExcludePolicy policy;
    ASSERT_EQ(policy.evaluate(CidrRange::ip_to_uint("192.0.2.1")),
              IpExcludeReason::none);
    ASSERT_EQ(policy.evaluate(CidrRange::ip_to_uint("203.0.113.1")),
              IpExcludeReason::none);
}

TEST(rule_engine_process_ip_exclude_is_scoped) {
    flat_tree tree;
    FILETIME ft = {};
    add_test_entry(tree, 4, 0, ft, L"System");
    add_test_entry(tree, 100, 4, ft, L"edge.exe");
    add_test_entry(tree, 200, 4, ft, L"chrome.exe");

    auto edge = make_rule("edge", "edge.exe");
    edge.dst_filter.exclude_cidrs = {CidrRange::parse("198.51.100.0/24")};
    auto chrome = make_rule("chrome", "chrome.exe");

    rule_engine_v3 engine;
    engine.set_auto_rules({edge, chrome});
    engine.apply_auto_rules(tree);

    auto ip = CidrRange::ip_to_uint("198.51.100.13");
    ASSERT_EQ(engine.ip_exclude_reason(tree, 100, ip), IpExcludeReason::rule);
    ASSERT_EQ(engine.ip_exclude_reason(tree, 200, ip), IpExcludeReason::none);
}

TEST(rule_engine_global_exclude_covers_manual_hijack) {
    flat_tree tree;
    FILETIME ft = {};
    add_test_entry(tree, 4, 0, ft, L"System");
    add_test_entry(tree, 100, 4, ft, L"manual.exe");

    rule_engine_v3 engine;
    engine.set_default_exclude_cidrs({"192.0.2.0/24"});
    engine.manual_hijack(tree, 100, 0);

    ASSERT_EQ(engine.ip_exclude_reason(tree, 100, CidrRange::ip_to_uint("192.0.2.3")),
              IpExcludeReason::global);
    ASSERT_EQ(engine.ip_exclude_reason(tree, 100, CidrRange::ip_to_uint("198.51.100.3")),
              IpExcludeReason::none);
}

TEST(rule_engine_hack_tree_inherits_ip_exclude) {
    flat_tree tree;
    FILETIME ft = {};
    add_test_entry(tree, 4, 0, ft, L"System");
    add_test_entry(tree, 100, 4, ft, L"edge.exe");
    add_test_entry(tree, 101, 100, ft, L"renderer.exe");

    auto edge = make_rule("edge", "edge.exe", true);
    edge.dst_filter.exclude_cidrs = {CidrRange::parse("203.0.113.0/24")};
    rule_engine_v3 engine;
    engine.set_auto_rules({edge});
    engine.apply_auto_rules(tree);

    ASSERT_EQ(engine.ip_exclude_reason(tree, 101, CidrRange::ip_to_uint("203.0.113.2")),
              IpExcludeReason::rule);
}

TEST(udp_tracker_policy_uses_network_order_destination) {
    auto tracker = std::make_unique<UdpPortTracker>();
    UdpTrackerEntry entry;
    auto policy = std::make_shared<IpExcludePolicy>();
    policy->global_cidrs = {CidrRange::parse("192.0.2.0/24")};
    entry.exclude_policy = policy;
    tracker->put(5353, entry);

    auto tracked = tracker->get(5353);
    ASSERT_TRUE(tracked.has_value());
    ASSERT_TRUE(tracked->exclude_policy != nullptr);

    const auto host_ip = CidrRange::ip_to_uint("192.0.2.4");
    ASSERT_EQ(udp_ip_exclude_reason(*tracked, htonl(host_ip)), IpExcludeReason::global);
    ASSERT_EQ(udp_ip_exclude_reason(*tracked, htonl(CidrRange::ip_to_uint("198.51.100.4"))),
              IpExcludeReason::none);
}

// ============================================================
// 7. SOCKS5 UDP session tests
// ============================================================

TEST(socks5_udp_binds_wildcard_and_sends_to_concrete_relay) {
    FakeSocks5UdpPeer peer(false);
    asio::io_context ioc;
    auto session = std::make_shared<clew::Socks5UdpSession>(
        ioc, "127.0.0.1", peer.proxy_port());

    ASSERT_TRUE(session->establish());
    ASSERT_TRUE(session->local_udp_endpoint().address().is_unspecified());
    ASSERT_TRUE(session->relay_endpoint().address().is_loopback());

    const std::vector<uint8_t> frame{0x05, 0x00, 0x01, 0x02, 0x03, 0x04};
    ASSERT_TRUE(run_udp_send(ioc, session, frame));
    ASSERT_EQ(peer.received(), frame);
}

TEST(socks5_udp_wildcard_relay_falls_back_to_proxy_host) {
    FakeSocks5UdpPeer peer(true);
    asio::io_context ioc;
    auto session = std::make_shared<clew::Socks5UdpSession>(
        ioc, "127.0.0.1", peer.proxy_port());

    ASSERT_TRUE(session->establish());
    ASSERT_TRUE(session->local_udp_endpoint().address().is_unspecified());
    ASSERT_EQ(session->relay_endpoint().address().to_string(),
              std::string("127.0.0.1"));

    const std::vector<uint8_t> frame{0x11, 0x22, 0x33};
    ASSERT_TRUE(run_udp_send(ioc, session, frame));
    ASSERT_EQ(peer.received(), frame);
}

TEST(socks5_udp_control_loss_marks_session_dead_and_send_fails) {
    FakeSocks5UdpPeer peer(false);
    asio::io_context ioc;
    auto session = std::make_shared<clew::Socks5UdpSession>(
        ioc, "127.0.0.1", peer.proxy_port());
    ASSERT_TRUE(session->establish());

    auto work = asio::make_work_guard(ioc);
    std::thread io_thread([&ioc] { ioc.run(); });
    const std::vector<uint8_t> first{0xaa, 0xbb};
    auto first_result = asio::co_spawn(
        ioc, session->async_send_udp(first), asio::use_future);
    ASSERT_TRUE(first_result.get());
    ASSERT_EQ(peer.received(), first);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (session->is_alive() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_FALSE(session->is_alive());

    auto second_result = asio::co_spawn(
        ioc, session->async_send_udp({0xcc}), asio::use_future);
    ASSERT_FALSE(second_result.get());

    session->close();
    work.reset();
    ioc.stop();
    io_thread.join();
}

// ============================================================
// 8. JSON round-trip tests
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
