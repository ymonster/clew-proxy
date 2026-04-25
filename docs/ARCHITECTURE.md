# Clew Architecture

Contributor-facing technical overview. For the end-user intro see [README.md](../README.md).

## Tech Stack

- **Language**: C++23 (concepts, C++20 coroutines for relay, `using enum`, fold expressions)
- **Build**: CMake + vcpkg
- **Core deps**: WinDivert (kernel traffic intercept), WebView2 (embedded browser UI)
- **Backend libs** (vcpkg): `quill` (logging), `nlohmann-json`, `cpp-httplib`, `asio` (standalone)
- **Frontend**: Vue 3 + TypeScript + shadcn-vue (reka-ui) + Tailwind CSS 4 + AG Grid + Monaco (JSON-only, lazy-loaded)
- **IPC**: HTTP API + SSE between the C++ backend and the WebView2 frontend
- **Config**: JSON (`clew.json`)
- **Target**: Windows 10 2004+ / 11 (requires `SetInterfaceDnsSettings`), administrator privileges

## Project Structure

The codebase is organized as a four-layer architecture: **domain** → **application services** → **transport**, with a **projection** layer materializing strand state for HTTP / SSE consumers. `clew::app` (`src/app.{hpp,cpp}`) is the single owner of the runtime object graph; `main.cpp` is the thin entry adapter (parse args, RAII guards for single-instance / debug console / Winsock, then construct + run the app).

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

  projection/                    - state holders bridging domain → transport
    process_projection.{hpp,cpp}     - tree_change_receiver: snapshot + SSE fanout
    config_sse_bridge.{hpp,cpp}      - config_store observer → SSE auto_rule_changed

  transport/                     - HTTP API server (cpp-httplib)
    http_api_server.{hpp,cpp}        - cpp-httplib server + 32-worker thread pool
    middleware.{hpp,cpp}             - CORS / OPTIONS / cache headers (post-routing)
    route_def.hpp                    - {method, pattern, handler} descriptor + http_method enum
    route_registry.{hpp,cpp}         - dispatcher: 3-tier exception catch + per-request log
    response_utils.{hpp,cpp}         - json body helpers (parse_json_body / write_json)
    sse_events.hpp                   - SSE event-name string constants
    sse_hub.{hpp,cpp}                - sink registry + broadcast across HTTP workers
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
  e2e_api_test.py                    - 21-test integration suite against a running clew
  run_all.py                         - admin-shell harness: launch + wait-ready + run + teardown
scripts/
  verify.sh                          - one-shot: build + 7 layering grep guards + run_all.py
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
GET    /api/processes              - Process tree (JSON snapshot from strand)
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
GET    /api/events                 - SSE stream
```

The engine is always-on while `clew.exe` runs — there is no start/stop control plane. WinDivert layers + acceptor + UDP relay + DNS manager are initialized at startup and torn down on shutdown.

SSE events: `process_update`, `process_exit`, `auto_rule_changed`.

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

## Build notes

- WinDivert + `SetInterfaceDnsSettings` both require administrator privileges. The UAC manifest is embedded via linker `/MANIFESTUAC:level='requireAdministrator'` (see `CMakeLists.txt`) — double-click triggers UAC prompt automatically.
- ~30 translation units after the three-layer refactor (was a single TU in the legacy header-only layout). `CMakeLists.txt` enables `/MP` for parallel compilation; first clean build is noticeably longer than the legacy version, incremental builds are fine.
- Frontend builds to static files served by cpp-httplib at runtime; dev mode uses Vite's proxy to port 18080.
- Kill `clew.exe` before rebuilding (MSVC LNK1104 error otherwise).
- `bash scripts/verify.sh` runs build + 7 layering grep guards + the 21-test e2e suite as a single command. Requires an administrator shell (clew.exe needs elevation).
