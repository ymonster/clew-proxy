# Clew Architecture

Contributor-facing technical overview. For the end-user intro see [README.md](../README.md).

## Tech Stack

- **Language**: C++23 (concepts, C++20 coroutines for relay, `using enum`, fold expressions)
- **Build**: CMake + vcpkg
- **Core deps**: WinDivert (kernel traffic intercept), WebView2 (embedded browser UI)
- **Backend libs** (vcpkg): `quill` (logging), `nlohmann-json`, `cpp-httplib`, `asio` (standalone)
- **Frontend**: Vue 3 + TypeScript + shadcn-vue (reka-ui) + Tailwind CSS 4 + AG Grid + Monaco (JSON-only, lazy-loaded)
- **IPC**: HTTP API for CRUD + WebView2 in-process PostMessage for backend → frontend push (replaces the previous SSE channel)
- **Config**: JSON (`clew.json`)
- **Target**: Windows 10 2004+ / 11 (requires `SetInterfaceDnsSettings`), administrator privileges

## Project Structure

The codebase is organized as a four-layer architecture: **domain** → **application services** → **transport**, with a **projection** layer materializing strand state for HTTP responders and the WebView2 push channel. `clew::app` (`src/app.{hpp,cpp}`) is the single owner of the runtime object graph; `main.cpp` is the thin entry adapter (parse args, RAII guards for single-instance / debug console / Winsock, then construct + run the app).

```
src/
  main.cpp                       - thin entry: WinMain/main → run_app() → clew::app
  app.{hpp,cpp}                  - composition root, owns ~30 subsystem members,
                                    explicit shutdown order in dtor

  common/                        - cross-layer utilities
    api_context.hpp                  - DI aggregate of service refs passed to handlers
    api_exception.{hpp,cpp}          - api_error enum + api_exception (throwable across layers)
    json_patch.hpp                   - apply_patch + field_binding template (whitelisted PATCH)
    process_tree_json.hpp            - shared process-tree → JSON serialization
    winsock_session.hpp              - RAII WSAStartup / WSACleanup
    single_instance_guard.hpp        - RAII single-instance mutex
    debug_console_session.hpp        - RAII AllocConsole / FreeConsole

  config/
    types.hpp                    - All data types: AutoRule, ProxyGroup, DnsConfig, ConfigV2
    config_manager.hpp           - JSON config persistence (clew.json)
    config_change_tag.hpp        - observer dispatch tag enum
    config_store.{hpp,cpp}       - thin wrapper: mutate(fn, tag) + observer fanout

  domain/                        - strand-bound application kernel
    strand_bound.hpp                 - concept-constrained query/command template
    tree_change_receiver.hpp         - listener interface (on_tree_changed / on_process_exit)
    process_tree_manager.{hpp,cpp}   - owns flat_tree + rule_engine + ETW driver

  services/                      - 8 application services (HTTP-facing logic)
    config_service / connection_service / group_service / icon_service /
    process_tree_service / rule_service / shell_service / stats_service

  projection/                    - state holders bridging domain → transport / UI
    process_projection.{hpp,cpp}     - tree_change_receiver: maintains atomic snapshot
                                       + pushes through frontend_push_sink with a
                                       100 ms strand-timer coalesce window for
                                       push_urgency::batched events
    config_sse_bridge.{hpp,cpp}      - config_store observer → push auto_rule_changed
                                       (file name retained for git history; the sink
                                       is now PostMessage, not SSE)

  transport/                     - HTTP API server + push channel interface
    http_api_server.{hpp,cpp}        - cpp-httplib server + 8-worker thread pool
    middleware.{hpp,cpp}             - CORS / OPTIONS / cache headers (post-routing)
    route_def.hpp                    - {method, pattern, handler} descriptor + http_method enum
    route_registry.{hpp,cpp}         - dispatcher: 3-tier exception catch + per-request log
    response_utils.{hpp,cpp}         - json body helpers (parse_json_body / write_json)
    frontend_push_sink.hpp           - push(event, json_body) interface; the
                                       projection layer calls it from any thread,
                                       webview_app marshals onto the UI thread
    handlers/                        - 9 thin route modules (one per resource group)

  core/                          - low-level infrastructure
    log.hpp                          - quill wrapper, PC_LOG_* macros + runtime set_log_level
    scoped_exit.hpp                  - unique_handle (Win32 HANDLE RAII) + scoped_exit<F>
    string_hash.hpp                  - transparent string_hash + string_map<V> alias
    port_tracker.hpp                 - atomic array[65536] mapping local port -> tracker entry
    windivert_socket.hpp             - TCP SOCKET SNIFF: intercepts connect(), writes PortTracker
    windivert_network.hpp            - TCP NETWORK reflection: reads PortTracker, redirects
    dns_forwarder.hpp                - UDP DNS listener, forwards via SOCKS5 UDP ASSOCIATE
    dns_manager.hpp                  - dns_forwarder lifecycle + system DNS state
    system_dns.hpp                   - Win32 SetInterfaceDnsSettings + dns_state.json persist

  process/                       - process discovery + tree
    flat_tree.hpp                    - vector<process_entry> + LC-RS indices, O(1) PID lookup
    etw_consumer.hpp                 - ETW real-time ProcessStart / ProcessStop consumer
    ntquery_snapshot.hpp             - Initial full process snapshot via NtQuerySystemInformation
    tcp_table.hpp / udp_table.hpp    - OS connection table queries

  rules/                         - auto-rule matching + traffic filtering
    rule_engine_v3.hpp               - flat_tree flag-based engine, no mutex
    traffic_filter.hpp               - CIDR / port destination filter

  proxy/                         - TCP relay
    acceptor.hpp                     - Asio TCP acceptor, spawns relay coroutines
    relay.hpp                        - C++20 coroutine bidirectional pipe + SOCKS5 handshake
    socks5_async.hpp                 - Async SOCKS5 handshake coroutine

  udp/                           - UDP relay (mirrors TCP, per-app-port sessions)
    windivert_socket_udp.hpp / windivert_network_udp.hpp
    udp_port_tracker.hpp / udp_session_table.hpp / udp_relay.hpp
    socks5_udp_manager.hpp / socks5_udp_session.hpp / socks5_udp_codec.hpp

  api/
    icon_cache.hpp                   - GDI+ icon extraction + PNG cache (AUMID-aware)

  ui/
    webview_app.hpp                  - Frameless WebView2 host + tray; WndProc dispatcher

frontend/                            - Vue 3 + TypeScript (built static files served by cpp-httplib)
tests/
  test_components.cpp                - Component-level unit tests (wildcard, flat_tree, etc.)
  e2e_api_test.py                    - 19-case HTTP integration suite (requests-based)
  run_all.py                         - admin-shell harness: launch + wait-ready + run + teardown
  playwright_e2e/
    poc_attach.py                    - Playwright + WebView2 CDP attach PoC
    run_pw.py                        - 5-case UI e2e suite: push reception, no SSE leak,
                                       DELETE roundtrip regression net, no polling
                                       under ETW load
scripts/
  verify.sh                          - one-shot: frontend build + cpp build + 7 layering
                                       grep guards + HTTP e2e + Playwright e2e
assets/
  clew.svg / clew.ico / clew.rc      - Embedded Windows icon
```

## Key Architectural Decisions

### Core runtime: single io_context + strand

- One `asio::io_context` with configurable worker threads (`io_threads`, default `hardware_concurrency() / 2`)
- One shared `strand` serializes all process tree + rule engine mutations (zero mutex in hot path)
- ETW events, process start/stop, rule changes all dispatched through the strand

### Process tree: ETW + NtQuery snapshot + Flat Tree

- Real-time ETW `ProcessStart` / `ProcessStop` instead of polling
- `ntquery_snapshot` provides the initial full tree; ETW maintains it incrementally
- `process_tree_manager` orchestrates: ETW + NtQuery + Flat Tree + Rule Engine

### Flat Tree with LC-RS (Left-Child Right-Sibling)

- `flat_tree.hpp`: `std::vector<process_entry>` + `std::unordered_map<DWORD, uint32_t>` side map
- Each entry stores `{pid, parent_pid, create_time, name_u8[780], parent_index, first_child_index, next_sibling_index, flags, group_id, cmdline_cache}`
- O(1) lookup by PID via side map, O(subtree) traversal via LC-RS indices
- `flags` field stores hijack state directly on the entry (no separate map)
- `group_id` field stores proxy group assignment (read by SOCKET layer in hot path)
- Tombstone + compact: dead entries marked, auto-compacted when tombstones > 20% alive

### Backend → frontend push: WebView2 PostMessage

Replaces the earlier SSE / `EventSource` channel. Push events flow:

1. A domain mutation (ETW process start/stop, manual hijack, rule reload) calls
   `process_tree_manager::notify_tree_changed(urgency)` on the strand.
2. `process_projection::on_tree_changed` rebuilds the atomic snapshot and
   pushes immediately (default build). The `push_urgency` hint distinguishes
   user actions (`immediate`) from background ETW (`batched`); see "Coalescing"
   below for the optional compile-time switch.
3. Projection calls `frontend_push_sink::push("process_update", json)`.
4. `webview_app::push` allocates a `push_payload` and posts a custom
   `WM_PUSH_TO_FRONTEND` to the UI thread, so cross-thread marshalling is
   explicit across HTTP workers, ETW threads, and the strand.
5. The UI-thread WndProc takes ownership, builds `{event, data}`, and calls
   `ICoreWebView2::PostWebMessageAsJson`.
6. Frontend's `notify.ts` listens on `chrome.webview` `message` events and
   updates the shared Vue refs (`tree`, etc.); components consume them
   directly.
7. Initial sync: when the frontend mounts (or when visibility transitions
   from hidden back to visible) it posts `{type: 'ready'}` back through
   `chrome.webview.postMessage`. The host invokes
   `process_projection::replay_to_frontend` to re-push the latest snapshot.

Why this transport:

- **In-process IPC**: no HTTP socket, no chunked SSE byte stream — V8's main
  thread is no longer busy parsing the push channel while it is also rendering.
- **No browser connection-limit interference**: HTTP/1.1 caps per-host
  connections (Chromium's default is 6); a long-lived SSE stream permanently
  occupied one of those slots, so bursts of concurrent CRUD calls had one
  fewer slot available. In-process IPC is independent of that pool entirely.
- **Simpler shutdown**: a single window message in flight at most, no
  long-lived HTTP connection to drain.

`/api/processes` (the snapshot endpoint that frontends used to poll) was
removed because the snapshot now arrives inside every `process_update` push;
nothing on the frontend needs to fetch the full tree any more. `/api/processes/:pid`
and `/api/processes/:pid/detail` are kept because they answer single-record
questions that the push channel does not duplicate.

#### Coalescing — opt-in compile switch (`CLEW_PROJECTION_COALESCE`)

Default build does **no** coalescing — every ETW event triggers an immediate
refresh + push. Snappy at any normal tree size: `process_tree_to_json_string`
costs ~2 µs per process, ETW arrival rate on a quiet box is typically <30/sec,
so strand utilisation stays under 1% even with 1000 live processes.

`cmake -DCLEW_PROJECTION_COALESCE=ON` enables a 100 ms refresh-coalesce
window for `batched` urgency. The fix-it-properly version (refresh **inside**
the timer callback, not per-event) is what's compiled in — bursts of hundreds
of ETW events collapse to a single refresh + push at the end of the window.
`immediate` urgency (user-driven) still bypasses the timer regardless.

When to enable: only if a real workload generates sudden +1000-process
bursts (large parallel build, runaway spawn) where the strand budget matters.
The trade-off is up to 100 ms of tree-update lag for batched events. For
characterisation use `tests/stress_etw.py` + `tests/stress_backend.py` (see
the "Build notes" section).

### Visibility gate — minimize / tray drops backend work

When the WebView2 host is hidden (window minimized or in the tray), the
backend stops doing tree-snapshot work entirely. Implementation:

1. `webview_app::set_visible(bool)` is the single chokepoint. It's called
   from `WM_SIZE` (SIZE_MINIMIZED → false, SIZE_RESTORED / SIZE_MAXIMIZED →
   true), `on_close` close-to-tray (false), and `restore_window` (true).
   It toggles `ICoreWebView2Controller::IsVisible` (so `document.visibilityState`
   in the embedded page tracks reality) and fires a registered
   `on_visibility_change_` callback. Idempotent — repeated calls with the
   same state are no-ops, which matters for `WM_SIZE` chatter.
2. `app::wire_observers` wires the callback to
   `process_projection::set_frontend_visible(bool)`, which stores into a
   `std::atomic<bool> frontend_visible_` (relaxed memory order — this is
   a hint, not a strict barrier).
3. `process_projection::on_tree_changed` checks the flag at the very top
   and early-returns when hidden. No `refresh_snapshot`, no PostMessage,
   no V8 wake-up. The strand is free to serve other work (in practice
   there's nothing else to serve while the user has the window away).
4. The frontend `useDocumentVisibility` composable mirrors the gate on the
   client side: the three remaining polled endpoints (`/api/stats`,
   `/api/tcp`, `/api/udp` via `fetchActivity` / `fetchConnections` /
   `fetchStatus`) suspend their `setInterval` while hidden.
5. When the host returns to visible, `set_visible(true)` does NOT replay
   automatically — instead, the frontend's `visibilitychange` handler
   re-posts `{type: 'ready'}`, which drives the existing replay path
   (`process_projection::replay_to_frontend`). The frontend gets a full
   fresh snapshot in one shot.

Net effect (measured under sustained 270 ETW events/sec at tree=1200):
strand utilisation 48% visible → 0.0% hidden, hijack wait_us tail 940 ms →
92 µs (any user CRUD waiting in the queue still completes; just nothing
new piling up). See `memory/MEMORY.md` for the v0.8.9 release notes.

### Frontend HTTP CRUD: empty `body: ''` on DELETE

This is independent of the push-transport switch above — they were two
separate problems that happened to ship in the same release.

cpp-httplib (as of v0.31, the version pinned by vcpkg) treats DELETE the same
as POST/PUT/PATCH: `expect_content()` returns true and the server's
`read_content_core` enters an `MSG_PEEK` block whenever the request has neither
`Content-Length` nor chunked `Transfer-Encoding`. With the default 100 MB
`payload_max_length` and 5 s `read_timeout_sec`, that peek waits the full
read timeout before the handler is even dispatched — even if the handler
reads no body. Browser `fetch` DELETE without an explicit body sends no
`Content-Length`, so an Unhack click stalled for ~5 s server-side before any
of our code ran.

The fix is on the client side. `frontend/src/api/client.ts` always passes
`body: ''` for DELETE-shaped CRUD calls (`unhijackProcess`, `deleteAutoRule`,
`unexcludePid`, `deleteProxyGroup`). The browser then emits
`Content-Length: 0`, cpp-httplib takes the fast path in `read_content` (length
0 → return immediately), and the handler runs in <2 ms. The server-side
alternative (`set_payload_max_length(SIZE_MAX)`) was avoided because it opens
the server to unbounded payload allocations; a one-line client change is the
proportional fix. See `memory/lesson_cpp_httplib_delete_5s.md` for the full
diagnosis trail.

### WinDivert dual-layer

- **SOCKET SNIFF layer** (`windivert_socket.hpp`): intercepts `connect()` via WinDivert SOCKET layer. Reads `flat_tree[pid].group_id` directly (O(1)). If hijacked, writes `{pid, group_id}` into `PortTracker[local_port]`. Runs via strand posting.
- **NETWORK reflection layer** (`windivert_network.hpp`): reads `PortTracker[src_port]`. If match: `swap(SrcAddr,DstAddr)` + `DstPort = redirect_port` + `Outbound = 0` (inbound reinject, streamdump pattern). Filter: `outbound and tcp and !loopback`. Runs in two dedicated blocking threads.

### PortTracker

- `std::array<TrackerSlot, 65536>` with each slot `atomic<bool> active` + `TrackerEntry{remote_addr, remote_port, group_id}`
- `alignas(64)` per slot to avoid false sharing (~4 MB heap)
- release/acquire semantics: SOCKET handler writes (strand), NETWORK workers read (blocking threads)
- Entries persist for connection lifetime, cleared on relay close

### C++20 coroutine relay

- `acceptor.hpp`: Asio TCP acceptor spawns `relay_session` coroutine per connection
- `relay.hpp`: bidirectional async pipe using `asio::co_spawn` + `async_read_some` / `async_write`
- `socks5_async.hpp`: SOCKS5 handshake as a coroutine (no blocking threads)

### Rule priority

1. **Manual rule** (PID exact match, sets `flags | MANUAL` on flat_tree entry) — highest
2. **Auto rule** (process name / cmdline match, config order) — sets `group_id`
3. **Default**: DIRECT (`group_id = 0`, no flags)

### Auto rule matching

- **`process_name`**: glob match (`*` any sequence, `?` single char, case-insensitive). Examples: `python.exe`, `curl*`
- **`cmdline_pattern`**: two modes, auto-selected by pattern content:
  - **Keyword mode** (no `*` or `?`): space-separated fragments, ALL must appear as case-insensitive substrings, order-independent. `udp_client` matches `python.exe udp_client.py --port 8080`; `udp_client 8080` also matches; `udp_client 9090` does not.
  - **Glob mode** (contains `*` or `?`): full wildcard match against the entire cmdline, order-sensitive. `*udp_client*8080*` matches; `*8080*udp_client*` does not.
- **Lazy cmdline query**: `cmdline` is fetched via `NtQueryInformationProcess` only when a rule has `cmdline_pattern` set AND `process_name` already matched. Cached in `process_entry.cmdline_cache` (sentinel `\x01` = queried but failed/empty). Zero overhead when no rule uses cmdline.

### Process tree logic for auto rules (hack_tree=true)

Find matching process → traverse to tree root (parent not matching same name) → set flags on root and all LC-RS descendants (including dynamically created children via ETW ProcessStart).

### PID recycling handling

- Windows recycles PIDs; `th32ParentProcessID` is unreliable
- ETW `ProcessStop` triggers immediate cleanup of flat_tree entry + PortTracker slot
- Flat-tree LC-RS pointers updated atomically on process exit

### Multi-proxy routing via proxy groups

- `ProxyGroup`: named proxy config with auto-increment `uint32_t id` (0 = default group)
- `AutoRule.proxy_group_id` references a group
- Relay coroutine reads group config from a shared map to determine SOCKS5 target

### UDP interception (mirrors TCP, per-port sessions)

- Same dual-layer architecture as TCP: `windivert_socket_udp` + `windivert_network_udp`
- Each app-level UDP port gets its own SOCKS5 UDP ASSOCIATE session (RFC 1928)
- Response routing via per-port session table — no cross-process bleed
- See `src/udp/` for all UDP-specific files

### DNS proxy (optional, off by default)

- User enables via Settings → DNS Proxy toggle
- **Forwarder mode** (only mode implemented): `dns_forwarder` listens UDP on `127.0.0.2:53`, forwards queries to upstream (default `8.8.8.8:53`) via SOCKS5 UDP ASSOCIATE using the first proxy group's endpoint
- `DnsManager` (src/core/dns_manager.hpp) orchestrates:
  - **System DNS auto-config**: on enable, saves original DNS per active IPv4 interface to `dns_state.json`, points interfaces to `127.0.0.2` via `SetInterfaceDnsSettings`
  - **Hot-reload**: Settings UI changes trigger `apply()`, which diffs new vs current config and starts / stops / restarts forwarder + updates system DNS accordingly, no restart required
  - **Crash recovery**: on startup, if `dns_state.json` exists it means the previous session exited abnormally — restore system DNS from file, then delete it
- Limitation: forwarder is UDP-only. Chromium TCP:53 fallback is not handled (not needed in practice when UDP works).

## API Endpoints

```
GET    /api/processes/:pid         - Single process info
GET    /api/processes/:pid/detail  - Process detail (cmdline, path)
GET    /api/hijack                 - List all hijacked PIDs
POST   /api/hijack/:pid            - Hijack a PID (manual rule)
DELETE /api/hijack/:pid            - Unhijack a PID
POST   /api/hijack/batch           - Batch hijack / unhijack
GET    /api/tcp                    - TCP connections (optionally ?pid=X)
GET    /api/udp                    - UDP connections
GET    /api/auto-rules             - List auto rules
POST   /api/auto-rules             - Create auto rule
PUT    /api/auto-rules/:id         - Update auto rule
DELETE /api/auto-rules/:id         - Delete auto rule
POST   /api/auto-rules/:id/exclude/:pid - Exclude PID from auto rule
DELETE /api/auto-rules/:id/exclude/:pid - Remove PID exclusion
GET    /api/proxy-groups           - List proxy groups
POST   /api/proxy-groups           - Create proxy group
PUT    /api/proxy-groups/:id       - Update proxy group
DELETE /api/proxy-groups/:id       - Delete proxy group
POST   /api/proxy-groups/:id/migrate - Migrate rules from one group to another before delete
POST   /api/proxy-groups/:id/test  - Measure latency to test_url
GET    /api/config                 - Get full config JSON
PUT    /api/config                 - Save full config JSON
GET    /api/stats                  - Counts (hijacked_pids, auto_rules_count)
GET    /api/env                    - Environment info
POST   /api/shell/browse-exe       - Open file dialog for .exe
POST   /api/shell/reveal           - Reveal file in Explorer
```

The engine is always-on while `clew.exe` runs — there is no start/stop control plane. WinDivert layers + acceptor + UDP relay + DNS manager are initialized at startup and torn down on shutdown.

Push events (delivered via `WM_PUSH_TO_FRONTEND` → `PostWebMessageAsJson`,
*not* over HTTP — see "Backend → frontend push" above):
- `process_update` — full process-tree snapshot, after the projection's
  100 ms coalesce window
- `auto_rule_changed` — `{action: string}`; tells the frontend to refetch
  rules-related state

## Config Schema (`clew.json`)

```json
{
  "version": 2,
  "default_proxy": { "type": "socks5", "host": "127.0.0.1", "port": 7890 },
  "proxy_groups": [
    {
      "id": 0,
      "name": "default",
      "host": "127.0.0.1",
      "port": 7890,
      "type": "socks5",
      "test_url": "http://www.gstatic.com/generate_204"
    }
  ],
  "next_group_id": 1,
  "default_exclude_cidrs": ["127.0.0.0/8", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16"],
  "auto_rules": [
    {
      "id": "...",
      "name": "...",
      "enabled": false,
      "process_name": "curl*",
      "cmdline_pattern": "",
      "image_path_pattern": "",
      "hack_tree": true,
      "protocol": "tcp",
      "proxy_group_id": 0,
      "dst_filter": {}
    }
  ],
  "ui": { "window_width": 1200, "window_height": 800, "dark_mode": true, "close_to_tray": false },
  "io_threads": 0,
  "log_level": "info",
  "dns": {
    "enabled": false,
    "mode": "forwarder",
    "upstream_host": "8.8.8.8",
    "upstream_port": 53,
    "listen_host": "127.0.0.2",
    "listen_port": 53
  }
}
```

## Build

- **Configure**: `cmake --preset windows-vcpkg` (uses `CMakePresets.json`)
- **Build Release**: `cmake --build build --config Release`
- **Build Debug**: `cmake --build build --config Debug`
- **Run** (admin required): `.\build\Release\clew.exe`
- **Frontend dev**: `cd frontend && npm run dev`
- **Frontend build**: `cd frontend && npm run build`
- **Opt-in: refresh-coalesce window** (perf experimentation only, default OFF):
  `cmake -DCLEW_PROJECTION_COALESCE=ON build` then rebuild. Enables a 100 ms
  refresh-coalesce timer for `batched` urgency events (see "Coalescing" in
  Key Architectural Decisions).

## Build notes

- WinDivert + `SetInterfaceDnsSettings` both require administrator privileges. The UAC manifest is embedded via linker `/MANIFESTUAC:level='requireAdministrator'` (see `CMakeLists.txt`) — double-click triggers UAC prompt automatically.
- ~30 translation units after the three-layer refactor (was a single TU in the legacy header-only layout). `CMakeLists.txt` enables `/MP` for parallel compilation; first clean build is noticeably longer than the legacy version, incremental builds are fine.
- Frontend builds to static files served by cpp-httplib at runtime; dev mode uses Vite's proxy to port 18080.
- **Launch with the right cwd**: clew's HTTP server uses relative path `./frontend/dist`. Launching the binary from outside the repo root (e.g. via Bash with `cwd=elsewhere`) silently serves whatever `./frontend/dist` resolves to in the launching shell's directory — easy way to get an empty / stale UI. Use `(cd <repo> && ./build/Release/clew.exe)` or `subprocess.Popen(..., cwd=<repo>)`.
- Kill `clew.exe` before rebuilding (MSVC LNK1104 error otherwise).
- `bash scripts/verify.sh` runs frontend build + cpp build + 7 layering grep guards + the HTTP e2e suite (`run_all.py`) + the Playwright + WebView2 e2e suite (`run_pw.py`) as a single command, each stage wrapped with a per-stage `timeout` so a single hung step aborts cleanly. Requires an administrator shell (clew.exe needs elevation), `npm` for the frontend build, and `uv` for the PEP 723 inline-deps Playwright runner.

### Performance harness (no Playwright)

For perf characterisation independent of the Playwright runner (which itself
competes with V8 main-thread push processing under load):

- `tests/stress_etw.py` — PEP 723 ETW churn driver. Maintains a target
  population of short-lived `cmd.exe /c ping ...` children so Windows
  continuously fires ProcessStart / ProcessStop events. Each child = 2 OS
  processes (cmd + ping) so `--target N` produces ~2N stress processes plus
  ambient. Knobs: `--target / --min-life / --max-life / --duration`.
- `tests/stress_backend.py` — pure-Python perf measurement. Spawns
  stress_etw, exercises hijack via HTTP at a configurable cadence, scans
  `clew.log` for instrumentation traces in the measured wall-clock window,
  reports per-stage timing distributions (refresh_us / push_us / strand
  wait_us / hijack RTT). Optional `--visibility-cycle` to drive minimize
  / restore mid-test via `WM_SYSCOMMAND` and bucket pre / hidden / post
  metrics — useful for verifying the visibility gate.

Neither harness is part of `verify.sh` (they're for ad-hoc characterisation,
not regression).
