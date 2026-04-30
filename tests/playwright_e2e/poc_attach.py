# /// script
# requires-python = ">=3.11"
# dependencies = [
#     "playwright>=1.49",
#     "requests>=2.31",
# ]
# ///
"""
Phase 0 PoC for the Playwright + WebView2 e2e plan.

Goal: prove we can launch clew.exe with the WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS
env var set, attach Playwright over CDP, run page.evaluate against the live
frontend, and tear it all down cleanly.

This script does *not* assert anything yet — it just prints findings. If it
runs to "PoC OK" we know Phase 1 (real test cases) is feasible.

Prereqs:
  - Build: cmake --build build --config Release --target clew
  - Frontend: cd frontend && npm run build
  - Playwright Chromium: uv run --script tests/playwright_e2e/poc_attach.py
    will auto-install playwright deps; you must also run once:
    `uv run --script tests/playwright_e2e/poc_attach.py --install-browser`
    or manually `python -m playwright install chromium`. CDP-only attach
    technically doesn't need the bundled Chromium, but Playwright still
    bootstraps it on first import on some setups.

Run from an admin shell (clew.exe needs elevation):
    uv run --script tests/playwright_e2e/poc_attach.py
"""

from __future__ import annotations

import argparse
import ctypes
import os
import pathlib
import subprocess
import sys
import tempfile
import time

import requests
from playwright.sync_api import sync_playwright


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
EXE = REPO_ROOT / "build" / "Release" / "clew.exe"
READY_URL = "http://127.0.0.1:18080/api/stats"
CDP_PORT = 9222
CDP_URL = f"http://localhost:{CDP_PORT}"
READY_TIMEOUT_S = 30.0


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--no-launch", action="store_true",
                        help="Use already-running clew.exe (must have CDP enabled)")
    args = parser.parse_args()

    if not is_elevated():
        print("ERROR: PoC requires admin shell (clew.exe needs elevation)",
              file=sys.stderr)
        return 2

    own_proc: subprocess.Popen | None = None

    if not args.no_launch:
        if is_clew_up():
            print("WARN: clew.exe already running — PoC will attach to it but "
                  "CDP may not be enabled. Use --no-launch to acknowledge.",
                  file=sys.stderr)
            return 1
        if not EXE.exists():
            print(f"ERROR: {EXE} not found. Build first.", file=sys.stderr)
            return 1

        # Per-run user-data folder so this PoC doesn't fight the user's
        # primary clew_webview_data state.
        data_dir = tempfile.mkdtemp(prefix="clew_pw_poc_")

        env = {
            **os.environ,
            "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS": f"--remote-debugging-port={CDP_PORT}",
            "WEBVIEW2_USER_DATA_FOLDER": data_dir,
        }

        print(f"[poc] launching {EXE.name} with CDP on :{CDP_PORT} "
              f"(data_dir={data_dir})")
        own_proc = subprocess.Popen(
            [str(EXE)],
            cwd=str(REPO_ROOT),
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
        )

        if not wait_clew_ready():
            print(f"ERROR: clew did not become ready (HTTP /api/stats) within "
                  f"{READY_TIMEOUT_S}s", file=sys.stderr)
            own_proc.kill()
            return 1
        print("[poc] clew HTTP ready")

        if not wait_cdp_ready():
            print(f"ERROR: CDP endpoint {CDP_URL} did not become reachable. "
                  "WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS may not be propagated.",
                  file=sys.stderr)
            own_proc.kill()
            return 1
        print(f"[poc] CDP endpoint {CDP_URL}/json/version reachable")

    rc = 1
    try:
        with sync_playwright() as p:
            print(f"[poc] connect_over_cdp({CDP_URL}) ...")
            browser = p.chromium.connect_over_cdp(CDP_URL)
            print(f"[poc]   browser.contexts = {len(browser.contexts)}")

            if not browser.contexts:
                print("ERROR: no browser contexts found", file=sys.stderr)
                return 1

            ctx = browser.contexts[0]
            print(f"[poc]   context.pages    = {len(ctx.pages)}")
            for i, pg in enumerate(ctx.pages):
                print(f"[poc]     page[{i}] url={pg.url!r} title={pg.title()!r}")

            if not ctx.pages:
                print("ERROR: no pages in context (frontend not loaded yet?)",
                      file=sys.stderr)
                return 1

            # Pick the first page that looks like our frontend (loaded from
            # 127.0.0.1:18080). Some WebView2 hosts also expose internal
            # about:blank pages.
            page = next(
                (pg for pg in ctx.pages if "127.0.0.1:18080" in pg.url),
                ctx.pages[0],
            )
            print(f"[poc] selected page: {page.url}")

            # Probe 1: chrome.webview should exist on a real WebView2 host
            has_webview = page.evaluate("typeof window.chrome?.webview")
            print(f"[poc] window.chrome.webview type = {has_webview!r}")

            # Probe 2: the title should be "Clew"
            title = page.title()
            print(f"[poc] title = {title!r}")

            # Probe 3: count process tree entries via DOM. The leftmost panel
            # has a `[data-testid]` we don't have yet; fall back to counting
            # nodes that look like ProcessTreeNode rows. Anything > 0 is fine
            # at PoC stage.
            row_count = page.evaluate("""
                () => document.querySelectorAll('aside .truncate.font-mono').length
            """)
            print(f"[poc] process-row-like DOM elements: {row_count}")

            # Probe 4: hook into the frontend's notify.tree ref via a global
            # bridge. App.vue / notify.ts don't expose it on window today;
            # we can still read length by querying a function call result.
            # If the bridge isn't there, log "n/a" and move on.
            tree_len = page.evaluate("""
                () => {
                    const wv = window.chrome?.webview
                    return { webview: !!wv, ua: navigator.userAgent }
                }
            """)
            print(f"[poc] page introspection: {tree_len}")

            # Probe 5: grab any console output that already buffered.
            # We attach a listener after the fact so this is best-effort.
            console_msgs: list[str] = []
            page.on("console", lambda m: console_msgs.append(f"{m.type}: {m.text}"))
            page.evaluate("() => console.log('poc-console-probe')")
            time.sleep(0.3)
            print(f"[poc] console msgs captured: {len(console_msgs)}")
            for m in console_msgs[:5]:
                print(f"[poc]   {m}")

            print("[poc] PoC OK — Playwright attach + page.evaluate works")
            rc = 0

    except Exception as e:
        print(f"ERROR: {type(e).__name__}: {e}", file=sys.stderr)
        rc = 1

    finally:
        if own_proc is not None:
            print("[poc] stopping clew.exe ...")
            subprocess.run(
                ["taskkill", "/F", "/PID", str(own_proc.pid)],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )

    return rc


if __name__ == "__main__":
    sys.exit(main())
