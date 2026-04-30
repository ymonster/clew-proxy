# Playwright + WebView2 e2e — Phase 0 PoC

This document describes the PoC that verifies Playwright can attach to the
running `clew.exe` WebView2 host over CDP and drive the frontend.

## Why this exists

`tests/e2e_api_test.py` only exercises the HTTP API. Anything that lives on
the Vue / WebView2 side (push handling, button visibility, fetch-network
behavior) is untested. The Phase 0 goal is to prove the attach pipeline
works; Phase 1 will add real test cases on top of it.

## How to run the PoC

```bash
# 1. Build clew (Release)
cmake --build build --config Release --target clew

# 2. Build the frontend
(cd frontend && npm run build)

# 3. Run the PoC from an admin shell (clew.exe needs elevation)
uv run --script tests/playwright_e2e/poc_attach.py
```

Expected output:

```
[poc] launching clew.exe with CDP on :9222 (data_dir=...)
[poc] clew HTTP ready
[poc] CDP endpoint http://localhost:9222/json/version reachable
[poc] connect_over_cdp(http://localhost:9222) ...
[poc]   browser.contexts = 1
[poc]   context.pages    = 1
[poc]     page[0] url='http://127.0.0.1:18080/' title='frontend'
[poc] selected page: http://127.0.0.1:18080/
[poc] window.chrome.webview type = 'object'
[poc] title = 'frontend'
[poc] process-row-like DOM elements: 14
[poc] page introspection: {'webview': True, 'ua': '...Edg/147.0.0.0'}
[poc] console msgs captured: 1
[poc]   log: poc-console-probe
[poc] PoC OK — Playwright attach + page.evaluate works
[poc] stopping clew.exe ...
```

If the script does not reach `PoC OK`, check the failing line:

| Stops at | Likely cause |
|---|---|
| `clew did not become ready` | clew.exe failed to bind 18080 (port in use, or admin failed) |
| `CDP endpoint did not become reachable` | `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` not propagating; ensure your local `webview_app.hpp` includes the env-var merge block (committed on `clew_postmsg`) |
| `no browser contexts found` | WebView2 has not finished initialization yet; the PoC's HTTP-ready check should normally cover this — try increasing the wait |
| `no pages in context` | The frontend page has not navigated yet; same fix as above |

## What the PoC enables

The build now honors `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS`. To open
DevTools manually for debugging, also pass the `--devtools` CLI flag:

```bash
WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9222 \
    build/Release/clew.exe --devtools
```

Then in another terminal, attach via Playwright:

```python
from playwright.sync_api import sync_playwright
with sync_playwright() as p:
    browser = p.chromium.connect_over_cdp("http://localhost:9222")
    page = browser.contexts[0].pages[0]
    print(page.evaluate("() => document.querySelectorAll('aside .truncate.font-mono').length"))
```

## What does NOT work the way you might expect

- The HTML `<title>` is `"frontend"`, not `"Clew"`. The C++-side
  `set_title(L"Clew")` controls only the OS window title, not the document
  title. **Don't assert on `page.title()`.** Use the URL instead.
- The visible ProcessTreeNode count in the DOM (~14) is much smaller than
  `notify.tree.value.length` (~376) because of virtual scrolling. Read tree
  state via `page.evaluate(...)` or by exposing a small bridge object on
  `window`, not by counting DOM nodes.
- CDP-only attach does NOT need `python -m playwright install chromium`.
  Playwright auto-bootstraps when you import it.
- Always set `WEBVIEW2_USER_DATA_FOLDER` to a per-run temp directory
  (the PoC does this) to avoid colliding with the user's daily
  `clew_webview_data/` profile.

## Next steps

Phase 1 will add:
- `test_push_after_hijack` — POST /api/hijack, verify Vue tree updated
- `test_t14_batch_single_push` — batch_hijack emits exactly 1 push
- `test_unhack_under_60ms` — regression net for the cpp-httplib DELETE 5s bug
- `test_no_event_source` — defends against SSE returning
- `test_no_request_storm` — defends against polling regressions

These will live next to `poc_attach.py` and run as part of `verify.sh`.
