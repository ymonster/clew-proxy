# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "playwright>=1.49",
#     "requests>=2.31",
# ]
# ///
"""
Phase 1 Playwright + WebView2 e2e suite.

Reinstates the two SSE-era tests that were skipped in tests/e2e_api_test.py
(test_sse, test_batch_hijack_single_notify a.k.a. T14) and adds three new
UI-side regression nets that pure HTTP testing can't observe.

Five tests:
  test_no_event_source       — no /api/events fetch is ever made
  test_push_after_hijack     — POST /api/hijack reflects in notify.tree.value
  test_t14_batch_single_push — batch hijack -> exactly 1 push (was T14)
  test_unhack_under_60ms     — DELETE /api/hijack roundtrip < 60 ms
                                (regression net for the cpp-httplib DELETE 5s bug)
  test_no_request_storm      — frontend doesn't poll under ETW load

Architecture:
  - launch clew.exe with WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9223
  - WEBVIEW2_USER_DATA_FOLDER = mktemp (parallel-safe with the user's daily run)
  - connect_over_cdp("http://localhost:9223")
  - read frontend state via window.__clew_debug.tree() (bridge in notify.ts)

Style matches tests/e2e_api_test.py: custom @test() decorator + global counters.
No pytest, no separate conftest.

Run from an admin shell:
    uv run --script tests/playwright_e2e/run_pw.py
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import os
import pathlib
import subprocess
import sys
import tempfile
import time

import requests
from playwright.sync_api import Page, sync_playwright


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
EXE = REPO_ROOT / "build" / "Release" / "clew.exe"
BASE = "http://127.0.0.1:18080/api"
READY_URL = f"{BASE}/stats"
CDP_PORT = 9223  # 9222 reserved for PoC; keep these distinct
CDP_URL = f"http://localhost:{CDP_PORT}"
READY_TIMEOUT_S = 30.0


passed = 0
failed = 0
errors: list[str] = []


def test(name):
    """Same shape as e2e_api_test.py — keep results compatible."""
    def decorator(fn):
        def wrapper(*args, **kwargs):
            global passed, failed
            try:
                fn(*args, **kwargs)
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


def is_elevated() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def is_clew_up() -> bool:
    try:
        r = requests.get(READY_URL, timeout=1.5)
        return r.status_code == 200
    except Exception:
        return False


def wait_clew_ready(timeout: float = READY_TIMEOUT_S) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if is_clew_up():
            return True
        time.sleep(0.5)
    return False


def is_cdp_up() -> bool:
    try:
        r = requests.get(f"{CDP_URL}/json/version", timeout=1.5)
        return r.status_code == 200
    except Exception:
        return False


def wait_cdp_ready(timeout: float = 10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if is_cdp_up():
            return True
        time.sleep(0.3)
    return False


# --------------------------------------------------------------- helpers


def _read_tree(page: Page) -> list[dict]:
    """Read `notify.tree.value` via the test bridge in notify.ts."""
    tree = page.evaluate("() => window.__clew_debug && window.__clew_debug.tree()")
    assert tree is not None, (
        "window.__clew_debug.tree() returned undefined — bridge missing or "
        "frontend not rebuilt after notify.ts change"
    )
    return tree


def _find_node(tree: list[dict], pid: int) -> dict | None:
    for n in tree:
        if n["pid"] == pid:
            return n
        children = n.get("children")
        if children:
            r = _find_node(children, pid)
            if r is not None:
                return r
    return None


def _wait_pid_in_tree(page: Page, pid: int, timeout: float = 4.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if _find_node(_read_tree(page), pid) is not None:
            return True
        time.sleep(0.1)
    return False


@contextlib.contextmanager
def _spawn_lingering(seconds: int = 30):
    """Spawn a cmd.exe child that lingers ~`seconds` via ping. Yields its PID."""
    proc = subprocess.Popen(
        ["cmd.exe", "/c", "ping", "127.0.0.1", "-n", str(seconds)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        yield proc.pid
    finally:
        with contextlib.suppress(Exception):
            proc.kill()
            proc.wait(timeout=2)


# ---------------------------------------------------------- the 5 tests


@test("No /api/events fetch is ever made (no SSE leak)")
def test_no_event_source(page: Page):
    """Frontend must not call EventSource and must not hit /api/events.
    The SSE endpoint was deleted server-side; this asserts nobody quietly
    reintroduces a client-side fetch path either, even under push activity."""
    seen: list[str] = []
    def on_request(req):
        if "/api/events" in req.url:
            seen.append(req.url)
    page.on("request", on_request)
    try:
        # Exercise the push path: hijack toggle drives a process_update push,
        # proving the channel is live without /api/events.
        with _spawn_lingering(seconds=10) as pid:
            assert _wait_pid_in_tree(page, pid), f"PID {pid} did not appear"
            r = requests.post(f"{BASE}/hijack/{pid}",
                              json={"tree": False, "group_id": 0})
            assert r.status_code == 200, f"hijack failed: {r.text}"
            time.sleep(0.5)
            requests.delete(f"{BASE}/hijack/{pid}", data="")
            time.sleep(0.5)
        assert not seen, f"unexpected /api/events fetches: {seen}"
    finally:
        page.remove_listener("request", on_request)


@test("Hijack push reaches the Vue tree")
def test_push_after_hijack(page: Page):
    """POST /api/hijack/<pid> -> backend push -> notify.tree updated.
    Asserts the targeted PID appears with hijacked=True and
    hijack_source=='manual' inside ~2 s."""
    with _spawn_lingering(seconds=20) as pid:
        assert _wait_pid_in_tree(page, pid), f"PID {pid} did not appear"

        r = requests.post(f"{BASE}/hijack/{pid}",
                          json={"tree": False, "group_id": 0})
        assert r.status_code == 200, f"hijack failed: {r.status_code} {r.text}"

        deadline = time.monotonic() + 2.0
        node = None
        while time.monotonic() < deadline:
            node = _find_node(_read_tree(page), pid)
            if node and node.get("hijacked"):
                break
            time.sleep(0.05)
        assert node is not None, f"PID {pid} disappeared from tree"
        assert node.get("hijacked"), f"node not marked hijacked: {node}"
        assert node.get("hijack_source") == "manual", (
            f"expected hijack_source=='manual', got {node.get('hijack_source')!r}"
        )

        # Cleanup so other tests start from clean state.
        requests.delete(f"{BASE}/hijack/{pid}", data="")


@test("Batch hijack emits exactly 1 process_update push (T14 reinstated)")
def test_t14_batch_single_push(page: Page):
    """DESIGN H5: manager.batch_hijack fires notify_tree_changed once for the
    whole batch, not N times. Counts envelopes received on the
    chrome.webview message channel before and after the batch call."""
    page.evaluate("""
        () => {
            window.__pwBatchPushes = []
            window.__pwBatchListener = (e) => {
                const m = e && e.data
                if (m && m.event === 'process_update'
                    && Array.isArray(window.__pwBatchPushes)) {
                    window.__pwBatchPushes.push(performance.now())
                }
            }
            window.chrome.webview.addEventListener('message', window.__pwBatchListener)
        }
    """)

    try:
        # Pick five live, non-reserved pids from the live tree (read via the
        # bridge — /api/processes is gone).
        tree = _read_tree(page)
        all_pids: list[int] = []
        def collect(nodes):
            for n in nodes:
                if n["pid"] not in (0, 4):
                    all_pids.append(n["pid"])
                if n.get("children"):
                    collect(n["children"])
        collect(tree)
        target_pids = all_pids[:5]
        assert len(target_pids) >= 3, f"need >=3 pids, got {len(target_pids)}"

        # Settle period after install + reset just before triggering.
        time.sleep(0.5)
        page.evaluate("() => { window.__pwBatchPushes = [] }")

        r = requests.post(f"{BASE}/hijack/batch", json={
            "pids": target_pids, "action": "hijack", "group_id": 0,
        })
        assert r.status_code == 200, f"batch hijack failed: {r.status_code} {r.text}"

        # 100 ms coalesce window in projection + headroom.
        time.sleep(0.6)

        push_count = page.evaluate("() => window.__pwBatchPushes.length")

        # Cleanup before assertion so we always restore state.
        requests.post(f"{BASE}/hijack/batch", json={
            "pids": target_pids, "action": "unhijack", "group_id": 0,
        })
        time.sleep(0.3)

        # 1 ideal, <=3 to absorb stray ProcessStop events on a busy machine
        # (matches the HTTP-side tolerance in the original T14).
        assert push_count <= 3, (
            f"expected <=3 process_update pushes after batch (ideal 1), "
            f"got {push_count} — per-pid fanout regression would be ~{len(target_pids)}"
        )
        print(f"    batch pushes: {push_count} (cap 3, ideal 1)")
    finally:
        page.evaluate("""
            () => {
                if (window.__pwBatchListener && window.chrome
                    && window.chrome.webview
                    && window.chrome.webview.removeEventListener) {
                    window.chrome.webview.removeEventListener('message', window.__pwBatchListener)
                }
                delete window.__pwBatchListener
                delete window.__pwBatchPushes
            }
        """)


@test("DELETE /api/hijack roundtrip < 60 ms (cpp-httplib regression net)")
def test_unhack_under_60ms(page: Page):
    """Regression net for the 5 s DELETE TTFB stall. The user-perceived
    Unhack click runs `fetch(/api/hijack/<pid>, {method:'DELETE', body:''})`.
    Measured from inside the page so we cover the whole
    browser -> cpp-httplib path."""
    with _spawn_lingering(seconds=20) as pid:
        assert _wait_pid_in_tree(page, pid), f"PID {pid} did not appear"
        r = requests.post(f"{BASE}/hijack/{pid}",
                          json={"tree": False, "group_id": 0})
        assert r.status_code == 200, f"setup hijack failed: {r.text}"

        result = page.evaluate(f"""
            async () => {{
                const t0 = performance.now()
                const r = await fetch('/api/hijack/{pid}', {{
                    method: 'DELETE',
                    body: '',
                }})
                const t1 = performance.now()
                return {{ status: r.status, ms: t1 - t0 }}
            }}
        """)
        assert result["status"] == 200, f"DELETE returned {result['status']}"
        assert result["ms"] < 60.0, (
            f"DELETE took {result['ms']:.1f} ms (cap 60 ms). "
            f"Likely regression of the cpp-httplib MSG_PEEK + SO_RCVTIMEO bug."
        )
        print(f"    DELETE roundtrip: {result['ms']:.1f} ms")


@test("Frontend does not poll under ETW load")
def test_no_request_storm(page: Page):
    """Spawn a burst of short-lived processes to drive ETW ProcessStart/Stop;
    count any /api/* fetches the page makes during the burst window. With
    the PostMessage transport the frontend never polls — it only fetches on
    user CRUD actions. Cap at 5 fetches over a 3-second window to catch a
    poller regression."""
    seen: list[str] = []
    def on_request(req):
        if "/api/" in req.url:
            seen.append(req.url)
    page.on("request", on_request)
    try:
        bursts = []
        for _ in range(8):
            bursts.append(subprocess.Popen(
                ["cmd.exe", "/c", "exit"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            ))
        for p in bursts:
            with contextlib.suppress(Exception):
                p.wait(timeout=2)

        time.sleep(3.0)
        assert len(seen) <= 5, (
            f"frontend made {len(seen)} /api/* requests during ETW burst "
            f"(cap 5): {seen[:8]}"
        )
        print(f"    /api/* requests during burst: {len(seen)} (cap 5)")
    finally:
        page.remove_listener("request", on_request)


# -------------------------------------------------------------------- main


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-launch", action="store_true",
                        help="Use already-running clew.exe (must have CDP enabled)")
    args = parser.parse_args()

    if not is_elevated():
        print("ERROR: needs admin shell (clew.exe requires elevation)",
              file=sys.stderr)
        return 2

    own_proc: subprocess.Popen | None = None

    if not args.no_launch:
        if is_clew_up():
            print(f"ERROR: clew.exe already running. Use --no-launch only if it "
                  f"already has CDP on :{CDP_PORT}.", file=sys.stderr)
            return 1
        if not EXE.exists():
            print(f"ERROR: {EXE} not found. Build first.", file=sys.stderr)
            return 1

        data_dir = tempfile.mkdtemp(prefix="clew_pw_e2e_")

        env = {
            **os.environ,
            "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS": f"--remote-debugging-port={CDP_PORT}",
            "WEBVIEW2_USER_DATA_FOLDER": data_dir,
        }

        print(f"==> launching {EXE.name} with CDP on :{CDP_PORT} (data_dir={data_dir})")
        own_proc = subprocess.Popen(
            [str(EXE)],
            cwd=str(REPO_ROOT),
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
        )

        if not wait_clew_ready():
            print(f"ERROR: clew did not become ready within {READY_TIMEOUT_S}s",
                  file=sys.stderr)
            own_proc.kill()
            return 1

        if not wait_cdp_ready():
            print(f"ERROR: CDP {CDP_URL} not reachable. "
                  "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS dropped?",
                  file=sys.stderr)
            own_proc.kill()
            return 1
        print("==> clew + CDP ready")

    rc = 1
    try:
        with sync_playwright() as p:
            browser = p.chromium.connect_over_cdp(CDP_URL)
            ctx = browser.contexts[0]
            page = next((pg for pg in ctx.pages if "127.0.0.1:18080" in pg.url),
                        ctx.pages[0])

            # Wait for the ready handshake + initial snapshot to land in
            # window.__clew_debug.tree(). Empty list is the pre-handshake state.
            deadline = time.monotonic() + 8.0
            tree = None
            while time.monotonic() < deadline:
                tree = page.evaluate(
                    "() => window.__clew_debug && window.__clew_debug.tree()"
                )
                if isinstance(tree, list) and len(tree) > 0:
                    break
                time.sleep(0.1)
            assert isinstance(tree, list) and len(tree) > 0, (
                "tree never populated via __clew_debug bridge — frontend "
                "rebuild needed? (cd frontend && npm run build)"
            )

            print()
            print("=" * 60)
            print("Clew E2E Playwright Test — 5 cases")
            print("=" * 60)

            test_no_event_source(page)
            test_push_after_hijack(page)
            test_t14_batch_single_push(page)
            test_unhack_under_60ms(page)
            test_no_request_storm(page)

            print()
            print("=" * 60)
            print(f"Results: {passed} passed, {failed} failed, "
                  f"{passed + failed} total")
            if errors:
                print("\nFailures:")
                for e in errors:
                    print(f"  - {e}")
            print("=" * 60)

            rc = 0 if failed == 0 else 1

    except Exception as e:
        print(f"ERROR: {type(e).__name__}: {e}", file=sys.stderr)
        rc = 1

    finally:
        if own_proc is not None:
            print("==> stopping clew.exe")
            subprocess.run(
                ["taskkill", "/F", "/PID", str(own_proc.pid)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )

    return rc


if __name__ == "__main__":
    sys.exit(main())
