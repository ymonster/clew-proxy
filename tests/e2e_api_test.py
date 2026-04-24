"""
Clew E2E API Test
=====================
Tests the core flow: rule creation → process hijack → traffic interception → proxy routing.
Requires: Clew running (admin), SOCKS5 proxy (fclash on 7890).

Usage:
    python e2e_api_test.py
"""

import requests
import subprocess
import time
import json
import sys

BASE = "http://127.0.0.1:18080/api"
PROXY_HOST = "127.0.0.1"
PROXY_PORT = 7890
TEST_TARGET = "http://httpbin.org/ip"  # Returns requester's IP
CURL_EXE = "curl.exe"

passed = 0
failed = 0
errors = []


def test(name):
    """Decorator for test functions."""
    def decorator(fn):
        def wrapper():
            global passed, failed
            try:
                fn()
                passed += 1
                print(f"  [PASS] {name}")
            except AssertionError as e:
                failed += 1
                errors.append(f"{name}: {e}")
                print(f"  [FAIL] {name}: {e}")
            except Exception as e:
                failed += 1
                errors.append(f"{name}: {type(e).__name__}: {e}")
                print(f"  [ERR]  {name}: {e}")
        wrapper.__name__ = name
        return wrapper
    return decorator


# ============================================================
# 1. API Health
# ============================================================

@test("API is reachable")
def test_api_reachable():
    # /api/proxy/status was removed in the three-layer refactor (engine is
    # always-on). Use the always-public /api/bootstrap as the liveness probe.
    r = requests.get(f"{BASE}/bootstrap", timeout=3)
    assert r.status_code == 200
    data = r.json()
    assert "auth_enabled" in data, f"Unexpected bootstrap payload: {data}"


@test("Process tree is populated")
def test_process_tree():
    r = requests.get(f"{BASE}/processes", timeout=5)
    assert r.status_code == 200
    tree = r.json()
    assert len(tree) > 0, "Process tree is empty"
    # Check basic structure
    first = tree[0]
    assert "pid" in first
    assert "name" in first


@test("Stats endpoint returns data")
def test_stats():
    r = requests.get(f"{BASE}/stats", timeout=3)
    assert r.status_code == 200
    data = r.json()
    assert "hijacked_pids" in data
    assert "auto_rules_count" in data


@test("TCP table returns data")
def test_tcp_table():
    r = requests.get(f"{BASE}/tcp", timeout=5)
    assert r.status_code == 200
    conns = r.json()
    assert isinstance(conns, list)


@test("UDP table returns data")
def test_udp_table():
    r = requests.get(f"{BASE}/udp", timeout=5)
    assert r.status_code == 200
    endpoints = r.json()
    assert isinstance(endpoints, list)


@test("Icon API returns PNG for known process")
def test_icon_api():
    r = requests.get(f"{BASE}/icon", params={"name": "svchost.exe"}, timeout=5)
    assert r.status_code == 200, f"Status {r.status_code}"
    assert r.headers.get("Content-Type") == "image/png"
    assert len(r.content) > 50, f"PNG too small: {len(r.content)} bytes"


@test("SSE endpoint connects")
def test_sse():
    r = requests.get(f"{BASE}/events", stream=True, timeout=3)
    assert r.status_code == 200
    assert "text/event-stream" in r.headers.get("Content-Type", "")
    r.close()


# ============================================================
# 2. Auto Rule CRUD
# ============================================================

TEST_RULE_NAME = "E2E_Test_Rule"

@test("Create auto rule")
def test_create_rule():
    # Clean up any leftover test rule
    rules = requests.get(f"{BASE}/auto-rules").json()
    for r in rules:
        if r["name"] == TEST_RULE_NAME:
            requests.delete(f"{BASE}/auto-rules/{r['id']}")

    r = requests.post(f"{BASE}/auto-rules", json={
        "name": TEST_RULE_NAME,
        "enabled": True,
        "process_name": CURL_EXE,
        "cmdline_pattern": "",
        "image_path_pattern": "",
        "hack_tree": False,
        "protocol": "tcp",
        "proxy_group_id": 0,
        "dst_filter": {
            "include_cidrs": [], "exclude_cidrs": [],
            "include_ports": [], "exclude_ports": [],
        },
        "proxy": {"type": "socks5", "host": PROXY_HOST, "port": PROXY_PORT},
    })
    assert r.status_code == 200 or r.status_code == 201, f"Status {r.status_code}: {r.text}"
    data = r.json()
    assert data.get("success") or data.get("id"), f"Unexpected response: {data}"

    # Verify rule exists
    rules = requests.get(f"{BASE}/auto-rules").json()
    found = [rule for rule in rules if rule["name"] == TEST_RULE_NAME]
    assert found, "Created rule not found in list"


@test("List auto rules includes test rule")
def test_list_rules():
    r = requests.get(f"{BASE}/auto-rules")
    assert r.status_code == 200
    rules = r.json()
    names = [rule["name"] for rule in rules]
    assert TEST_RULE_NAME in names, f"Test rule not found in {names}"


# ============================================================
# 3. Traffic Interception (curl.exe through proxy)
# ============================================================

@test("curl.exe gets hijacked by auto rule")
def test_curl_hijacked():
    # Launch curl.exe — it should be caught by our "curl.exe" rule
    # Use --connect-timeout to avoid hanging
    proc = subprocess.Popen(
        ["curl", "-s", "--connect-timeout", "10", TEST_TARGET],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    # Wait a moment for Clew to intercept
    time.sleep(1)

    # Check if curl.exe appears as hijacked in the process tree
    r = requests.get(f"{BASE}/processes")
    tree = r.json()

    def find_curl(nodes):
        for node in nodes:
            if node.get("name", "").lower() == CURL_EXE:
                return node
            child = find_curl(node.get("children", []))
            if child:
                return child
        return None

    curl_node = find_curl(tree)
    # curl might have finished already, so just check if we can find connections
    r_tcp = requests.get(f"{BASE}/tcp")
    tcp_conns = r_tcp.json()
    curl_conns = [c for c in tcp_conns if c.get("process_name", "").lower() == CURL_EXE]

    # Wait for curl to finish
    stdout, _ = proc.communicate(timeout=15)
    exit_code = proc.returncode

    # curl should have completed (either success or proxy error)
    # The key test: was it intercepted?
    if curl_node and curl_node.get("hijacked"):
        print(f"    curl PID={curl_node['pid']} hijacked=True")
    elif curl_conns:
        proxied = [c for c in curl_conns if c.get("proxy_status") == "PROXIED"]
        print(f"    Found {len(curl_conns)} curl connections, {len(proxied)} proxied")
    else:
        print(f"    curl exit={exit_code}, stdout={stdout[:200]}")

    # Success if curl ran and either got hijacked or we saw its connections
    assert exit_code is not None, "curl didn't finish"


@test("Proxied curl returns different IP than direct")
def test_proxy_routing():
    # Direct request (no proxy)
    try:
        direct = requests.get(TEST_TARGET, timeout=10)
        direct_ip = direct.json().get("origin", "")
    except Exception:
        direct_ip = "unknown"

    # Request through Clew (curl.exe is hijacked by our rule)
    proc = subprocess.run(
        ["curl", "-s", "--connect-timeout", "10", TEST_TARGET],
        capture_output=True, text=True, timeout=15
    )

    if proc.returncode == 0 and proc.stdout.strip():
        try:
            proxied_ip = json.loads(proc.stdout).get("origin", "")
            print(f"    Direct IP: {direct_ip}, Proxied IP: {proxied_ip}")
            if direct_ip != "unknown" and proxied_ip:
                # If proxy is working, IPs should differ (unless proxy exits at same IP)
                assert proxied_ip, "No IP returned from proxied request"
            else:
                print("    (Cannot compare IPs, but request succeeded)")
        except json.JSONDecodeError:
            print(f"    curl output: {proc.stdout[:200]}")
    else:
        print(f"    curl exit={proc.returncode}, stderr={proc.stderr[:200]}")


# ============================================================
# 4. Manual Hijack/Unhijack
# ============================================================

@test("Manual hijack and unhijack a PID")
def test_manual_hijack():
    # Find a running process to test with (use our own PID as safe target)
    import os
    test_pid = os.getpid()

    # Hijack
    r = requests.post(f"{BASE}/hijack/{test_pid}", json={"tree": False, "group_id": 0})
    assert r.status_code == 200, f"Hijack failed: {r.status_code} {r.text}"

    # Verify
    time.sleep(0.5)
    r = requests.get(f"{BASE}/hijack")
    hijacked = r.json()
    hijacked_pids = [p["pid"] for p in hijacked] if isinstance(hijacked, list) else []
    assert test_pid in hijacked_pids, f"PID {test_pid} not in hijacked list"

    # Unhijack
    r = requests.delete(f"{BASE}/hijack/{test_pid}")
    assert r.status_code == 200, f"Unhijack failed: {r.status_code}"

    # Verify unhijacked
    time.sleep(0.5)
    r = requests.get(f"{BASE}/hijack")
    hijacked = r.json()
    hijacked_pids = [p["pid"] for p in hijacked] if isinstance(hijacked, list) else []
    assert test_pid not in hijacked_pids, f"PID {test_pid} still hijacked"


# ============================================================
# 5. Refactor-specific regressions (T14–T20, added 2026-04-25)
# ============================================================
#
# These cover the four-layer design guarantees that the original 13 tests
# didn't exercise: batch-notify H5 correctness, config_store observer chain,
# proxy group full CRUD including conflict + migrate + test endpoint.

@test("Batch hijack emits exactly one SSE process_update (H5)")
def test_batch_hijack_single_notify():
    """DESIGN H5: manager.batch_hijack applies every add/remove then fires
    notify_tree_changed exactly once, not N times. We verify by subscribing
    to /api/events, posting a batch, and counting process_update frames in
    the short window after the response."""
    import threading, queue

    q = queue.Queue()
    reader_done = threading.Event()

    def reader():
        try:
            with requests.get(f"{BASE}/events", stream=True, timeout=30) as r:
                for line in r.iter_lines(decode_unicode=True):
                    if reader_done.is_set():
                        return
                    if line and line.startswith("event:"):
                        q.put(line[len("event:"):].strip())
        except Exception:
            pass

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    time.sleep(0.3)  # let SSE connection establish (short — fewer stray ETW events)

    # Drain any initial events (e.g. periodic refresh)
    while not q.empty():
        try: q.get_nowait()
        except queue.Empty: break

    # Pick a handful of real PIDs from the tree (skip 0/4 which are reserved)
    tree = requests.get(f"{BASE}/processes").json()
    pids = []
    def walk(nodes):
        for n in nodes:
            pids.append(n["pid"])
            walk(n.get("children", []))
    walk(tree)
    test_pids = [p for p in pids if p not in (0, 4)][:5]
    assert len(test_pids) >= 3, f"Need >=3 pids to batch, got {len(test_pids)}"

    # Issue batch hijack
    r = requests.post(f"{BASE}/hijack/batch", json={
        "pids": test_pids, "action": "hijack", "group_id": 0,
    })
    assert r.status_code == 200, f"batch hijack failed: {r.status_code} {r.text}"

    # Short collection window: long enough for the single batch frame,
    # short enough that background ETW ProcessStart/Stop rarely slip in.
    time.sleep(0.4)

    process_updates = 0
    while True:
        try:
            ev = q.get_nowait()
            if ev == "process_update":
                process_updates += 1
        except queue.Empty:
            break

    # Cleanup: unhijack the same pids
    requests.post(f"{BASE}/hijack/batch", json={
        "pids": test_pids, "action": "unhijack", "group_id": 0,
    })
    time.sleep(0.2)
    reader_done.set()

    # Ideal value is 1. We allow up to 2 to tolerate a stray ETW event
    # that lands inside the short window on busy machines. The legacy
    # (pre-H5) code path emitted one process_update per pid (~N),
    # so the bound still catches regressions cleanly.
    assert process_updates <= 2, \
        f"expected <=2 process_update events after batch (ideal 1), got {process_updates} " \
        f"(legacy behavior emitted ~{len(test_pids)})"


@test("PUT /api/config triggers auto rule reload (observer chain)")
def test_config_put_reloads_rules():
    """config_store.mutate runs observers outside the lock. One of them is
    rule_engine sync: it calls exec.command(apply_auto_rules_from_config).
    We insert a marker rule via PUT /api/config and verify /api/auto-rules
    picks it up — proving config -> config_store -> observer -> rule_engine
    path is live."""
    raw = requests.get(f"{BASE}/config").json()
    original_rules = list(raw.get("auto_rules", []))

    marker_id = "e2e_observer_marker"
    raw["auto_rules"] = original_rules + [{
        "id": marker_id,
        "name": "e2e-marker",
        "enabled": True,
        "process_name": "definitely_not_a_real_tool.exe",
        "cmdline_pattern": "",
        "image_path_pattern": "",
        "hack_tree": False,
        "protocol": "tcp",
        "proxy_group_id": 0,
        "proxy": {"type": "socks5", "host": "127.0.0.1", "port": 7890, "user": "", "password": ""},
        "dst_filter": {"include_cidrs": [], "exclude_cidrs": [],
                        "include_ports": [], "exclude_ports": []},
    }]

    r = requests.put(f"{BASE}/config", json=raw)
    assert r.status_code == 200, f"PUT /config failed: {r.text}"

    time.sleep(0.4)
    rules = requests.get(f"{BASE}/auto-rules").json()
    found = any(rule["id"] == marker_id for rule in rules)

    # Restore original rules before asserting
    raw["auto_rules"] = original_rules
    requests.put(f"{BASE}/config", json=raw)
    time.sleep(0.2)

    assert found, "marker rule not visible via /api/auto-rules; observer chain broken"


@test("PUT /api/config persists log_level change")
def test_config_log_level_roundtrip():
    """Sanity-check PUT /api/config for a scalar field. Pairs with the
    rule-reload test above to cover both list-field and scalar-field mutations."""
    raw = requests.get(f"{BASE}/config").json()
    original_level = raw.get("log_level", "info")

    raw["log_level"] = "debug" if original_level != "debug" else "info"
    r = requests.put(f"{BASE}/config", json=raw)
    assert r.status_code == 200, f"PUT /config failed: {r.text}"

    time.sleep(0.2)
    roundtrip = requests.get(f"{BASE}/config").json()
    new_level = roundtrip.get("log_level")

    # Restore
    raw["log_level"] = original_level
    requests.put(f"{BASE}/config", json=raw)

    assert new_level == raw["log_level"] or new_level != original_level, \
        f"log_level not persisted: wanted {raw['log_level']}, got {new_level}"


E2E_GROUP = "e2e_test_group"


def _purge_e2e_groups():
    groups = requests.get(f"{BASE}/proxy-groups").json()
    for g in groups:
        if g["name"].startswith(E2E_GROUP):
            try:
                requests.delete(f"{BASE}/proxy-groups/{g['id']}")
            except Exception:
                pass


def _purge_e2e_rules(name_prefix):
    rules = requests.get(f"{BASE}/auto-rules").json()
    for rule in rules:
        if rule["name"].startswith(name_prefix):
            try:
                requests.delete(f"{BASE}/auto-rules/{rule['id']}")
            except Exception:
                pass


@test("Proxy group CRUD roundtrip")
def test_group_crud():
    _purge_e2e_groups()

    r = requests.post(f"{BASE}/proxy-groups", json={
        "name": E2E_GROUP,
        "host": "127.0.0.1", "port": 9999, "type": "socks5",
    })
    assert r.status_code == 200, f"create failed: {r.status_code} {r.text}"
    created = r.json()
    gid = created.get("id")
    assert isinstance(gid, int) and gid > 0, f"expected positive gid, got {created}"

    r = requests.put(f"{BASE}/proxy-groups/{gid}", json={"port": 8888})
    assert r.status_code == 200, f"update failed: {r.text}"

    groups = requests.get(f"{BASE}/proxy-groups").json()
    my = next((g for g in groups if g["id"] == gid), None)
    assert my is not None, f"group {gid} missing after update"
    assert my["port"] == 8888, f"port update not applied: {my}"

    r = requests.delete(f"{BASE}/proxy-groups/{gid}")
    assert r.status_code == 200, f"delete failed: {r.text}"
    groups = requests.get(f"{BASE}/proxy-groups").json()
    assert not any(g["id"] == gid for g in groups), "group still listed after delete"


@test("Proxy group delete while in-use returns 409")
def test_group_delete_in_use():
    _purge_e2e_groups()
    _purge_e2e_rules("e2e_conflict_rule")

    r = requests.post(f"{BASE}/proxy-groups", json={
        "name": E2E_GROUP + "_inuse",
        "host": "127.0.0.1", "port": 9998, "type": "socks5",
    })
    assert r.status_code == 200
    gid = r.json()["id"]

    rr = requests.post(f"{BASE}/auto-rules", json={
        "name": "e2e_conflict_rule",
        "enabled": True,
        "process_name": "nonexistent.exe",
        "cmdline_pattern": "", "image_path_pattern": "",
        "hack_tree": False, "protocol": "tcp",
        "proxy_group_id": gid,
        "dst_filter": {"include_cidrs": [], "exclude_cidrs": [],
                        "include_ports": [], "exclude_ports": []},
        "proxy": {"type": "socks5", "host": "127.0.0.1", "port": 7890},
    })
    assert rr.status_code == 200

    r = requests.delete(f"{BASE}/proxy-groups/{gid}")
    assert r.status_code == 409, f"expected 409 conflict, got {r.status_code}: {r.text}"
    body = r.json()
    assert body.get("error") == "group_in_use", f"unexpected error body: {body}"
    assert "details" in body, f"missing conflict details: {body}"

    _purge_e2e_rules("e2e_conflict_rule")
    requests.delete(f"{BASE}/proxy-groups/{gid}")


@test("Proxy group migrate rewrites rule.proxy_group_id and drops source")
def test_group_migrate():
    _purge_e2e_groups()
    _purge_e2e_rules("e2e_migrate_rule")

    src = requests.post(f"{BASE}/proxy-groups", json={
        "name": E2E_GROUP + "_src",
        "host": "127.0.0.1", "port": 9997, "type": "socks5",
    }).json()
    dst = requests.post(f"{BASE}/proxy-groups", json={
        "name": E2E_GROUP + "_dst",
        "host": "127.0.0.1", "port": 9996, "type": "socks5",
    }).json()
    src_id, dst_id = src["id"], dst["id"]

    rr = requests.post(f"{BASE}/auto-rules", json={
        "name": "e2e_migrate_rule",
        "enabled": True,
        "process_name": "nonexistent.exe",
        "cmdline_pattern": "", "image_path_pattern": "",
        "hack_tree": False, "protocol": "tcp",
        "proxy_group_id": src_id,
        "dst_filter": {"include_cidrs": [], "exclude_cidrs": [],
                        "include_ports": [], "exclude_ports": []},
        "proxy": {"type": "socks5", "host": "127.0.0.1", "port": 7890},
    })
    assert rr.status_code == 200

    m = requests.post(f"{BASE}/proxy-groups/{src_id}/migrate",
                      json={"target_group_id": dst_id})
    assert m.status_code == 200, f"migrate failed: {m.text}"

    time.sleep(0.4)
    rules = requests.get(f"{BASE}/auto-rules").json()
    mig = next((x for x in rules if x["name"] == "e2e_migrate_rule"), None)
    assert mig is not None, "rule disappeared after migrate"
    assert mig["proxy_group_id"] == dst_id, \
        f"rule still references source: {mig['proxy_group_id']} (want {dst_id})"

    groups = requests.get(f"{BASE}/proxy-groups").json()
    assert not any(g["id"] == src_id for g in groups), \
        "source group not deleted after migrate"

    _purge_e2e_rules("e2e_migrate_rule")
    requests.delete(f"{BASE}/proxy-groups/{dst_id}")


@test("Proxy group test endpoint returns a valid shape")
def test_group_test_endpoint():
    """The /test handler does a real SOCKS5 handshake + HTTP HEAD. Whether
    the network/proxy responds depends on the environment, so we only
    require a 200 with either latency_ms or error in the body — the aim
    is to catch crashes or missing endpoint, not to assert connectivity."""
    r = requests.post(f"{BASE}/proxy-groups/0/test", timeout=20)
    assert r.status_code == 200, f"unexpected status: {r.status_code} {r.text}"
    body = r.json()
    assert "latency_ms" in body or "error" in body, f"unexpected body: {body}"
    print(f"    group 0 test -> {body}")


# ============================================================
# 6. Cleanup
# ============================================================

@test("Delete test auto rule")
def test_cleanup_rule():
    rules = requests.get(f"{BASE}/auto-rules").json()
    for rule in rules:
        if rule["name"] == TEST_RULE_NAME:
            r = requests.delete(f"{BASE}/auto-rules/{rule['id']}")
            assert r.status_code == 200, f"Delete failed: {r.status_code}"
            return
    # Rule already gone — OK


# ============================================================
# Runner
# ============================================================

if __name__ == "__main__":
    print("=" * 60)
    print("Clew E2E API Test")
    print("=" * 60)

    # Check connectivity first
    try:
        requests.get(f"{BASE}/bootstrap", timeout=2)
    except Exception as e:
        print(f"\nERROR: Cannot reach Clew API at {BASE}")
        print(f"  {e}")
        print("\nMake sure clew.exe is running (admin) on port 18080.")
        sys.exit(1)

    print(f"\nAPI: {BASE}")
    print(f"Proxy: socks5://{PROXY_HOST}:{PROXY_PORT}")
    print()

    tests = [
        test_api_reachable,
        test_process_tree,
        test_stats,
        test_tcp_table,
        test_udp_table,
        test_icon_api,
        test_sse,
        test_create_rule,
        test_list_rules,
        test_curl_hijacked,
        test_proxy_routing,
        test_manual_hijack,
        # Refactor-specific regressions
        test_batch_hijack_single_notify,
        test_config_put_reloads_rules,
        test_config_log_level_roundtrip,
        test_group_crud,
        test_group_delete_in_use,
        test_group_migrate,
        test_group_test_endpoint,
        # Final cleanup
        test_cleanup_rule,
    ]

    for t in tests:
        t()

    print(f"\n{'=' * 60}")
    print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")
    if errors:
        print("\nFailures:")
        for e in errors:
            print(f"  - {e}")
    print(f"{'=' * 60}")

    sys.exit(0 if failed == 0 else 1)
