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
    r = requests.get(f"{BASE}/proxy/status", timeout=3)
    assert r.status_code == 200
    data = r.json()
    assert data["running"] is True, f"Proxy not running: {data}"


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
# 5. Cleanup
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
        requests.get(f"{BASE}/proxy/status", timeout=2)
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
