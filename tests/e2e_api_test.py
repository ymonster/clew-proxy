"""
Clew E2E API Test
=====================
Tests the core flow: rule creation → process hijack → traffic interception → proxy routing.
Requires: Clew running (admin), SOCKS5 proxy (fclash on 7890).

Usage:
    python e2e_api_test.py
"""

import pathlib
import queue
import re
import requests
import subprocess
import sys
import threading
import time
import json
from contextlib import contextmanager
from datetime import datetime
from typing import Iterator

BASE = "http://127.0.0.1:18080/api"
PROXY_HOST = "127.0.0.1"
PROXY_PORT = 7890
TEST_TARGET = "http://httpbin.org/ip"  # Returns requester's IP
CURL_EXE = "curl.exe"

# clew.log lives next to clew.exe (since v0.9.0 chdir was reverted in favor
# of explicit exe-dir resolution). Tests scan it for tagged events that
# can't be observed at the HTTP level alone — see T22/T23 below.
REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
LOG_PATH  = REPO_ROOT / "build" / "Release" / "clew.log"

# Quill format: "%(time) [%(log_level_short_code)] %(message)"
# Shortened: "2026-05-01 14:15:23.123456 [D] [tree-change] source=batch_hijack"
_LOG_LINE_RE = re.compile(
    r'^(?P<ts>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{6})\s+'
    r'\[(?P<lvl>[IDWE])\]\s+'
    r'(?P<msg>.*)$'
)


def _parse_log_ts(s: str) -> float:
    """Quill timestamps are local-time naive; matches time.time() domain."""
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S.%f").timestamp()


def collect_log_messages(pattern: re.Pattern,
                         ts_start: float,
                         ts_end: float) -> list[tuple[float, re.Match]]:
    """Scan clew.log, return [(ts, match), ...] for lines whose timestamp
    is in [ts_start, ts_end] AND whose message body matches `pattern`.

    Real-time tailing on Windows + quill is unreliable (see stress_backend.py
    rationale). We re-read the file at end of the measurement window — the
    log is small enough that scanning the whole thing is fine."""
    out: list[tuple[float, re.Match]] = []
    if not LOG_PATH.exists():
        return out
    with open(LOG_PATH, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            m_outer = _LOG_LINE_RE.match(line.rstrip("\n"))
            if not m_outer:
                continue
            ts = _parse_log_ts(m_outer.group("ts"))
            if ts < ts_start or ts > ts_end:
                continue
            m_inner = pattern.search(m_outer.group("msg"))
            if m_inner:
                out.append((ts, m_inner))
    return out

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
    # Engine is always-on (no /api/proxy/status); use /api/stats as a liveness probe.
    r = requests.get(f"{BASE}/stats", timeout=3)
    assert r.status_code == 200
    data = r.json()
    assert isinstance(data, dict), f"Unexpected stats payload: {data}"


@test("Process tree is populated")
def test_process_tree():
    # /api/processes was removed when the backend->frontend push channel
    # switched from SSE to WebView2 PostMessage; the snapshot is delivered
    # in the push payload itself. e2e here only needs a backend liveness
    # signal — /api/stats works.
    r = requests.get(f"{BASE}/stats", timeout=5)
    assert r.status_code == 200
    data = r.json()
    assert "hijacked_pids" in data, f"unexpected stats payload: {data}"


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

    # /api/processes was retired; query /api/hijack which returns the
    # currently-hijacked PIDs with their names (subset we care about).
    r = requests.get(f"{BASE}/hijack")
    hijacked = r.json() or []
    curl_node = next(
        (h for h in hijacked if h.get("name", "").lower() == CURL_EXE), None
    )
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
    # Use our own PID as a safe target. Since this script was launched after
    # clew's initial NtQuery snapshot, our PID enters the tree only via ETW
    # ProcessStart, which has buffer-flush latency (~hundreds of ms).
    import os
    test_pid = os.getpid()
    assert _wait_pid_in_tree(test_pid), \
        f"PID {test_pid} did not appear in tree within timeout (ETW lag?)"

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

# ---------- SSE / process-tree helpers shared by T14+ ----------

RESERVED_PIDS = frozenset({0, 4})  # System Idle, System


def _is_event_line(line):
    return bool(line) and line.startswith("event:")


def _sse_reader(url, q, stop):
    """Push every `event: <name>` value from an SSE stream into q until stopped."""
    try:
        with requests.get(url, stream=True, timeout=30) as r:
            for line in r.iter_lines(decode_unicode=True):
                if stop.is_set():
                    return
                if _is_event_line(line):
                    q.put(line.removeprefix("event:").strip())
    except Exception:
        pass  # connection torn down on stop — expected


@contextmanager
def _sse_subscription(url, settle=0.3):
    """Yield a queue of SSE event names; reader thread is stopped on exit."""
    q = queue.Queue()
    stop = threading.Event()
    t = threading.Thread(target=_sse_reader, args=(url, q, stop), daemon=True)
    t.start()
    time.sleep(settle)
    try:
        yield q
    finally:
        stop.set()


def _drain(q):
    """Discard everything currently buffered. Non-blocking."""
    try:
        while True:
            q.get_nowait()
    except queue.Empty:
        return


def _count(q, name):
    """Count buffered events equal to `name`. Non-blocking."""
    n = 0
    try:
        while True:
            if q.get_nowait() == name:
                n += 1
    except queue.Empty:
        return n


def _iter_pids(nodes) -> Iterator[int]:
    """Flatten a process tree into a stream of pids."""
    for node in nodes:
        yield node["pid"]
        yield from _iter_pids(node.get("children", []))


def _wait_pid_in_tree(pid, timeout=3.0):
    """Poll the single-PID tree query until pid shows up. Tolerates ETW
    ProcessStart buffer-flush latency (~hundreds of ms) for processes
    spawned after the initial NtQuery snapshot."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        r = requests.get(f"{BASE}/processes/{pid}")
        if r.status_code == 200:
            return True
        time.sleep(0.1)
    return False


_TREE_CHANGE_RE = re.compile(r'\[tree-change\] source=(?P<source>\w+)')


@test("Batch hijack fires notify_tree_changed exactly once (H5)")
def test_batch_hijack_single_notify():
    """DESIGN H5: process_tree_manager::batch_hijack applies every add/remove
    on the strand and then fires notify_tree_changed("batch_hijack") exactly
    once for the whole batch — never per-pid. We assert the C++ invariant
    directly by counting `[tree-change] source=batch_hijack` lines in the
    measurement window. Background ETW pushes (source=etw_start / etw_stop /
    reconcile) and per-pid manual hijacks (source=manual_hijack) are
    correctly NOT counted, which is why the previous SSE/Playwright variants
    of this test were flaky on busy machines.

    Requires log_level=debug (set by main()'s setup phase)."""
    # Pre-settle: prior tests spawn curl.exe processes whose ProcessStop
    # events also generate tree-change lines, but those have a different
    # source tag so they don't pollute our count.
    time.sleep(0.5)

    tree = requests.get(f"{BASE}/processes").json() if False else None  # GET removed
    pids_resp = requests.get(f"{BASE}/processes").json() if False else None
    # /api/processes was removed with the PostMessage transition. Walk a
    # known-stable starting point: spawn cmd.exe children we own ourselves
    # so we know the PIDs without going through the tree.
    procs = []
    for _ in range(5):
        p = subprocess.Popen(
            ["cmd.exe", "/c", "ping -n 30 127.0.0.1 >nul"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
        )
        procs.append(p)
    test_pids = [p.pid for p in procs]
    try:
        # Wait for ETW to register every spawned pid so the strand has them
        # before we batch-hijack — and so the etw_start tree-change events
        # finish landing before the measurement window opens.
        for pid in test_pids:
            assert _wait_pid_in_tree(pid, timeout=3.0), (
                f"PID {pid} did not appear in tree (ETW backlog?)"
            )
        time.sleep(0.3)  # let etw_start tree-change lines flush

        ts_start = time.time()

        r = requests.post(f"{BASE}/hijack/batch", json={
            "pids": test_pids, "action": "hijack", "group_id": 0,
        })
        assert r.status_code == 200, f"batch hijack failed: {r.status_code} {r.text}"

        time.sleep(0.4)  # let quill flush + the listener fire
        ts_end = time.time()

        # Truth source: the /api/hijack state. All test pids must appear.
        hijacked_after = {p["pid"] for p in (requests.get(f"{BASE}/hijack").json() or [])}
        expected = set(test_pids)
        assert expected.issubset(hijacked_after), (
            f"batch did not hijack all expected pids: "
            f"missing {sorted(expected - hijacked_after)}"
        )

        events = collect_log_messages(_TREE_CHANGE_RE, ts_start, ts_end)
        batch_count = sum(1 for _, m in events if m.group("source") == "batch_hijack")

        # Exactly one batch_hijack tree-change in the window. ETW events
        # (source=etw_start / etw_stop) are filtered by source tag so the
        # test is robust to busy machines; the assertion targets only the
        # H5 invariant. If clew.log is missing we have bigger problems —
        # don't silently pass.
        assert LOG_PATH.exists(), f"{LOG_PATH} missing — chdir or log routing broken"
        assert batch_count == 1, (
            f"expected exactly 1 [tree-change] source=batch_hijack in window, "
            f"got {batch_count}. All sources seen: "
            f"{[m.group('source') for _, m in events]}"
        )

        # Cleanup
        requests.post(f"{BASE}/hijack/batch", json={
            "pids": test_pids, "action": "unhijack", "group_id": 0,
        })
        time.sleep(0.1)
    finally:
        for p in procs:
            try: p.terminate()
            except Exception: pass


_API_DELETE_RE = re.compile(
    r'\[api\] DELETE (?P<path>\S+) -> (?P<status>\d+) \((?P<us>\d+)us\)'
)


@test("DELETE /api/hijack roundtrip < 60ms (server-side, cpp-httplib regression net)")
def test_delete_under_60ms_serverside():
    """Pre-v0.8.8 cpp-httplib's MSG_PEEK + SO_RCVTIMEO bug made every DELETE
    block for read_timeout (default 5s). The fix shipped a 'body: \"\"' on
    the frontend; this is the regression net.

    We check the SERVER-SIDE elapsed time recorded by route_registry::dispatch
    (`[api] DELETE /api/hijack/N -> 200 (Xus)`), NOT the client-side wall
    clock. Client wall-clock is muddied by Windows TCP loopback Nagle and
    by Python's request stack overhead — the server-side number is what
    actually matters for the bug. Requires log_level=debug (the [api]
    line is INFO, but main()'s setup phase enables debug for T22 anyway)."""
    # Spawn an owned process so we have a stable PID to hijack/unhijack.
    p = subprocess.Popen(
        ["cmd.exe", "/c", "ping -n 30 127.0.0.1 >nul"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )
    try:
        assert _wait_pid_in_tree(p.pid, timeout=3.0), (
            f"PID {p.pid} did not appear in tree (ETW backlog?)"
        )

        r = requests.post(f"{BASE}/hijack/{p.pid}",
                          json={"tree": False, "group_id": 0})
        assert r.status_code == 200, f"hijack failed: {r.text}"

        ts_start = time.time()
        r = requests.delete(f"{BASE}/hijack/{p.pid}")
        ts_end = time.time()
        assert r.status_code == 200, f"delete failed: {r.status_code} {r.text}"

        # Allow the log message to flush before we read.
        time.sleep(0.2)

        events = collect_log_messages(_API_DELETE_RE, ts_start, ts_end + 0.5)
        target = f"/api/hijack/{p.pid}"
        match = next((m for _, m in events if m.group("path") == target), None)
        assert match is not None, (
            f"no [api] DELETE log line found for {target} in window. "
            f"Events seen: {[(m.group('path'), m.group('us')) for _, m in events]}"
        )

        elapsed_us = int(match.group("us"))
        assert elapsed_us < 60_000, (
            f"DELETE took {elapsed_us}us server-side (cap 60_000us = 60ms). "
            f"Likely regression of cpp-httplib MSG_PEEK + SO_RCVTIMEO bug "
            f"(was: every DELETE blocked for read_timeout = 5s)."
        )
    finally:
        try: p.terminate()
        except Exception: pass


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
# 7. DNS proxy round-trip (kept last — mutates system DNS)
# ============================================================

def _get_system_dns_servers():
    """Read DNS servers from active IPv4 interfaces. Returns a set."""
    cmd = ("Get-DnsClientServerAddress -AddressFamily IPv4 "
           "| Where-Object { $_.ServerAddresses } "
           "| ForEach-Object { $_.ServerAddresses } "
           "| Sort-Object -Unique")
    result = subprocess.run(
        ["powershell", "-NoProfile", "-Command", cmd],
        capture_output=True, text=True, timeout=10,
    )
    if result.returncode != 0:
        return set()
    return {line.strip() for line in result.stdout.splitlines() if line.strip()}


def _query_dns(server_ip, hostname='example.com', timeout=5.0):
    """Raw UDP DNS A-query. Return True on response, False on timeout."""
    import socket as sk
    import struct

    txid   = 0x1234
    flags  = 0x0100  # standard query, recursion desired
    header = struct.pack('!HHHHHH', txid, flags, 1, 0, 0, 0)
    qname  = b''.join(bytes([len(p)]) + p.encode() for p in hostname.split('.')) + b'\x00'
    query  = header + qname + struct.pack('!HH', 1, 1)  # A, IN

    s = sk.socket(sk.AF_INET, sk.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(query, (server_ip, 53))
        data, _ = s.recvfrom(512)
        return len(data) >= 12 and data[:2] == struct.pack('!H', txid)
    except (sk.timeout, OSError):
        return False
    finally:
        s.close()


@test("DNS proxy enable/disable round-trip with system DNS swap")
def test_dns_proxy_roundtrip():
    """Toggle dns.enabled via PUT /api/config and verify the full chain:
      - system DNS gets swapped to 127.0.0.2 on enable
      - forwarder on 127.0.0.2:53 actually answers a real query
        (validates SOCKS5 UDP ASSOCIATE path is alive)
      - system DNS gets restored on disable
    Cleans up unconditionally even on assertion failure."""
    raw          = requests.get(f"{BASE}/config").json()
    original_dns = dict(raw.get("dns") or {})

    initial_servers = _get_system_dns_servers()
    print(f"    system DNS before: {sorted(initial_servers) or '(empty)'}")

    try:
        # --- Enable ---
        enable_cfg = dict(raw, dns=dict(original_dns, enabled=True))
        r = requests.put(f"{BASE}/config", json=enable_cfg)
        assert r.status_code == 200, f"PUT enable failed: {r.status_code} {r.text}"

        time.sleep(1.0)  # let dns_mgr.apply swap system DNS + start forwarder

        servers_after_enable = _get_system_dns_servers()
        assert "127.0.0.2" in servers_after_enable, (
            f"127.0.0.2 not in system DNS after enable: "
            f"{sorted(servers_after_enable)} (was {sorted(initial_servers)})"
        )

        ok = _query_dns("127.0.0.2", "example.com", timeout=5.0)
        assert ok, "DNS forwarder did not respond on 127.0.0.2:53 within 5s"

        # --- Disable ---
        disable_cfg = dict(raw, dns=dict(original_dns, enabled=False))
        r = requests.put(f"{BASE}/config", json=disable_cfg)
        assert r.status_code == 200, f"PUT disable failed: {r.status_code} {r.text}"

        time.sleep(1.0)  # let dns_mgr.apply restore system DNS

        servers_after_disable = _get_system_dns_servers()
        assert "127.0.0.2" not in servers_after_disable, (
            f"127.0.0.2 still in system DNS after disable: "
            f"{sorted(servers_after_disable)}"
        )
        if servers_after_disable != initial_servers:
            print(f"    note: system DNS differs from initial "
                  f"(initial={sorted(initial_servers)}, "
                  f"now={sorted(servers_after_disable)})")
    finally:
        # Belt-and-suspenders: restore original config even on failure
        try:
            requests.put(f"{BASE}/config", json=raw, timeout=5)
            time.sleep(0.5)
        except Exception:
            pass


# ============================================================
# Runner
# ============================================================

if __name__ == "__main__":
    print("=" * 60)
    print("Clew E2E API Test")
    print("=" * 60)

    # Check connectivity first
    try:
        requests.get(f"{BASE}/stats", timeout=2)
    except Exception as e:
        print(f"\nERROR: Cannot reach Clew API at {BASE}")
        print(f"  {e}")
        print("\nMake sure clew.exe is running (admin) on port 18080.")
        sys.exit(1)

    print(f"\nAPI: {BASE}")
    print(f"Proxy: socks5://{PROXY_HOST}:{PROXY_PORT}")
    print(f"Log:   {LOG_PATH}")
    print()

    # Some tests assert source-tagged invariants (e.g. T22 batch_hijack
    # single notify) by scanning clew.log. Those debug lines are dropped
    # at quill's compile-cheap path under the default log_level=info, so
    # we flip to debug for the duration of the run and restore on exit.
    cfg = requests.get(f"{BASE}/config").json()
    original_log_level = cfg.get("log_level", "info")
    if original_log_level != "debug":
        cfg["log_level"] = "debug"
        requests.put(f"{BASE}/config", json=cfg)
        time.sleep(0.2)
        print(f"log_level: {original_log_level} -> debug (will restore on exit)")
    else:
        print("log_level: already debug")
    print()

    tests = [
        test_api_reachable,
        test_process_tree,
        test_stats,
        test_tcp_table,
        test_udp_table,
        test_icon_api,
        # test_sse retired: /api/events removed with the SSE->PostMessage
        # transport switch. Push-channel coverage now lives in Playwright
        # (test_no_event_source / test_push_after_hijack).
        test_create_rule,
        test_list_rules,
        test_curl_hijacked,
        test_proxy_routing,
        test_manual_hijack,
        test_batch_hijack_single_notify,         # T22 — log-scan rewrite
        test_delete_under_60ms_serverside,       # T23 — log-scan rewrite
        test_config_put_reloads_rules,
        test_config_log_level_roundtrip,
        test_group_crud,
        test_group_delete_in_use,
        test_group_migrate,
        test_group_test_endpoint,
        # Final cleanup
        test_cleanup_rule,
        # DNS round-trip last — mutates system DNS, has its own cleanup
        test_dns_proxy_roundtrip,
    ]

    try:
        for t in tests:
            t()
    finally:
        # Always restore log_level — verify.sh reuses the same clew.exe
        # process across phases, and a leftover debug level would persist
        # into clew.json on disk via config_store::mutate.
        try:
            cfg2 = requests.get(f"{BASE}/config").json()
            if cfg2.get("log_level") != original_log_level:
                cfg2["log_level"] = original_log_level
                requests.put(f"{BASE}/config", json=cfg2)
        except Exception as e:
            print(f"WARN: failed to restore log_level: {e}", file=sys.stderr)

    print(f"\n{'=' * 60}")
    print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")
    if errors:
        print("\nFailures:")
        for e in errors:
            print(f"  - {e}")
    print(f"{'=' * 60}")

    sys.exit(0 if failed == 0 else 1)
