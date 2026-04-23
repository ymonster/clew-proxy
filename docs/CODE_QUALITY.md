# Code Quality Policy

Summary of static-analysis findings and the project's decision for each
class of issue. Used as the reference for SonarCloud reviews and for new
contributors wondering why certain issues are marked "Accepted".

Scanner: **SonarCloud (Automatic Analysis)**
Scope: `src/`, `frontend/src/`, `tests/`, `scripts/`, `assets/`
Excluded: `WinDivert-2.2.2-A/**`, `src/ui/third_party/**`, `build/**`,
`frontend/dist/**`, `frontend/node_modules/**`, `docs/images/**`,
`**/*.min.js` — see [`sonar-project.properties`](../sonar-project.properties).

Note: Automatic Analysis reads exclusions from the SonarCloud UI
(Administration → Analysis Scope), **not** from `sonar-project.properties`.
The file is kept for future migration to CI-based analysis.

---

## Rule decisions

Shorthand: **Fixed** = code changed; **Accepted** = marked "Accept" in
SonarCloud UI with the comment shown below; **TODO** = deferred work with
a tracked plan.

### Fixed in bulk (mechanical replacements)

| Rule | Count | Action |
|------|-------|--------|
| cpp:S5018 (BLOCKER) | 2 | Add explicit `noexcept` move ctor to `flat_tree`, `AutoRule` — affects `std::vector` resize path |
| cpp:S1048 (BUG) | 2 | Destructor `~process_tree_manager` / `~DnsManager` wrapped in try/catch + `noexcept` |
| cpp:S6185 | 14 | `a + b + std::to_string(c)` → `std::format("{}...", ...)` — single-alloc, C++20 idiomatic |
| cpp:S6171 | 12 | `m.count(k)` / `m.find(k) == m.end()` → `m.contains(k)` — C++20 |
| cpp:S6168 | 10 | `std::thread` → `std::jthread` for members with matching `close()` / `stop()` owner methods |
| cpp:S134 (partial) | 5 | Extract helpers: `entry_matches_rule` (guard clauses), `resolve_proxy_filter_status`, `exe_path_from_handle`, `pump_pending_messages`, `parse_dns_qname` |
| cpp:S1659 | 8 | `int a, b;` → one declaration per line |
| Web:S6853 | 14 | Introduce shadcn-vue `<Label>` (reka-ui primitive) + Vue 3.5 `useId()` for explicit label/control association |
| cpp:S1231 | 13 | `malloc`/`calloc`/`free` → RAII via `std::vector<std::byte>` for variable-size WinAPI structs (ETW / `GetExtendedTcpTable` / `GetExtendedUdpTable`); `_dupenv_s` → `GetEnvironmentVariableA` |
| cpp:S3608 (partial) | 5 | `[&]` → explicit captures on stored/async callbacks in `main.cpp` |
| cpp:S5350 (partial) | 3+2 | Add `const` to read-only reference/pointer variables in `flat_tree.hpp` (prior batch) and `http_api_server` (`rules` in hijack GET, outer `cfg` in migrate validate). |
| cpp:S3574 (partial) | 1 | Remove redundant `-> char` on single-line lambda |
| cpp:S6391 (partial) | 1 | `async_send_udp(const std::vector<uint8_t>&)` → pass-by-value + `std::move` at call sites. Idiomatic C++20 coroutine buffer handoff: zero-copy via move, eliminates dangling risk if caller ever passes an rvalue. |
| cpp:S1874 | 7 | Deprecated CRT functions → Microsoft-canonical `_s` variants. `sscanf` → `sscanf_s`; `freopen` → `freopen_s`; `strncpy`/`wcsncpy`/`wcscpy` → `strncpy_s`/`wcsncpy_s`/`wcscpy_s` with `std::size(dst)` + `_TRUNCATE` or explicit count. Windows-only project, Annex K is fine. |
| cpp:S6009 | 7 | `const std::string&` / `const std::wstring&` read-only params → `std::string_view` / `std::wstring_view`. Sites: `http_api_server::set_static_dir`, `system_dns::parse_guid_string`, `rule_engine_v3::exclude_pid`/`unexclude_pid`/`should_proxy_protocol`, `webview_app::set_title`, test helper `make_rule`. Eliminates implicit `std::string` construction from string literals; C++23 `operator=(sv)` handles member assignment. |
| cpp:S6494 | 3 | `snprintf(buf, N, fmt, ...)` → `std::format(...)`. Sites: `CidrRange::uint_to_ip` (IP dotted-decimal); WinDivert filter string building in `windivert_socket::open` / `windivert_socket_udp::open`. Removes fixed-size stack buffer; WinDivert filter still passed as `filter.c_str()`. |
| cpp:S6012 | 3 | `std::lock_guard<std::mutex> lock(m);` → `std::lock_guard lock(m);` via CTAD. `http_api_server` SSE sync sites. |
| cpp:S1066 | 3 | Merge nested `if (A) { if (B) ... }` into `if (A && B) ...`. Sites: `rule_engine_v3` tree-match guard, `http_api_server` connection filter, `webview_app` WM_NCCALCSIZE maximize detection. |
| cpp:S1709 | 2 | Single-arg ctor → `explicit`. `process_tree_manager(asio::io_context&)`, `webview_app(const std::wstring&, int, int)`. Prevents accidental implicit conversions. |
| cpp:S6032 | 2 | WinDivert recv loop: `auto addr_copy = addr; asio::post(strand_, [this, addr_copy]{ ... });` → direct value-capture `asio::post(strand_, [this, addr]{ ... });`. Eliminates intermediate copy (the lambda already copies via default-value capture). |
| cpp:S6165 | 2 | `v.erase(std::remove_if(...), v.end())` / manual iterator erase-loop → `std::erase_if(v, pred)` (C++20). Sites: `http_api_server` group migration, `udp_session_table::cleanup_expired`. |
| cpp:S5506 | 2 | `socks5_udp_manager::get_or_create`: `std::lock_guard` + manual `mu_.unlock()` / `mu_.lock()` around a callback → `std::unique_lock` with `lk.unlock()`. Fixes latent UB if callback throws (dtor would unlock already-unlocked mutex); also removes the unnecessary re-acquire before return. |
| cpp:S1481 + S1854 | 2+2 | Remove unused variables / dead stores: `etw_consumer` `version` (declared from event header but never used); test_components `re_idx` (add_entry return value unused in PID-recycle scenario test). |
| cpp:S1659 | 2 | Split `uint16_t a, b;` / `std::string a, b;` → one identifier per line. `http_api_server` two sites. |
| typescript:S7773 | 5 | `parseInt(...)` → `Number.parseInt(...)` (global → namespaced). `ProxiesTab.vue`, `RuleEditorDialog.vue`. |
| typescript:S7764 | 2 of 3 | `self` / `window` (where type augmentation is not required) → `globalThis`. Kept `window.chrome` in `App.vue` (the `chrome.webview` property lives on the `Window` interface augmentation, not `globalThis`). |
| typescript:S7735 | 3 | Invert negated null-check conditions. `App.vue`: `if (pid != null) { ... } else { ... }` → `if (pid == null) { ...; return }` early-return. `client.ts` two ternaries: `pid != null ? a : b` → `pid == null ? b : a`. |
| typescript:S3358 | 3 | Extract nested ternaries. `ConfigEditor.vue` error-type mapping → if/else if chain. `NetworkActivities.vue` state-color mapping → extracted `stateDotColor()` helper. |
| cpp:S108 + S2486 (partial) | 4+4 | Replace empty `catch (...) {}` bodies with `PC_LOG_ERROR`/`PC_LOG_WARN`/`PC_LOG_DEBUG` diagnostics. Sites: `DnsManager` / `process_tree_manager` destructors (noexcept guard), `http_api_server` manual_count strand_sync, `socks5_udp_session::watch_tcp_control`. Both rules eliminated in one fix per site. |
| typescript:S7721 | 2 | Extract inner `on` / `off` helpers in `sse.ts::useSSE` to module scope as `sseOn` / `sseOff`. Avoids re-creating the closures per `useSSE()` call. |
| python:S1172 | 1 | `release.py::package_zip` unused `new_tag: str` parameter removed (only used in staging-path construction at call site, not inside the function). |
| python:S1481 | 1 | `tests/e2e_api_test.py` `stderr` unused in `proc.communicate()` → `_`. |
| python:S3457 | 2 | `tests/e2e_api_test.py` f-strings without replacement fields → plain strings. |
| python:S1192 | 2 | `tests/e2e_api_test.py` extracted `CURL_EXE` constant; `TEST_TARGET` already a constant, now used in place of repeated URL literal. |
| cpp:S1172 | 1 | `test_components` `visit_descendants` callback unused `idx` param → anonymous param `uint32_t`. |
| cpp:S5997 | 1 | `http_api_server` SSE sync sites: `std::lock_guard` → `std::scoped_lock` (C++17 recommended form; deadlock-safe if ever extended to multiple mutexes). Applied via replace_all to all 3 sites. |
| cpp:S4962 | 1 | `main.cpp::is_elevated` `HANDLE token = NULL` → `nullptr`. |
| cpp:S7034 | 1 | `rule_engine_v3::cmdline_match` `lower_cmdline.find(fragment) == std::string::npos` → `!lower_cmdline.contains(fragment)` (C++23 `std::string::contains`). |
| cpp:S6030 | 1 | `icon_cache::get_icon_png` `cache_.emplace(key, png)` → `cache_.try_emplace(key, png)` (semantic: don't overwrite if present). |
| cpp:S5950 | 1 | `system_dns::enumerate_active_interfaces` `std::unique_ptr<char[]>(new char[N])` → `std::make_unique<char[]>(N)`. |
| cpp:S6177 | 1 | `http_api_server` auth middleware: `using enum httplib::Server::HandlerResponse;` at lambda scope; 6 references collapse to bare `Unhandled` / `Handled`. |
| cpp:S1103 | 1 | Auth-middleware comment `// for all /api/* routes` → `// for all /api/... routes` (avoids `/*` that resembles a block-comment start). |
| cpp:S3562 | 1 | `webview_app::WndProc` tray/command switch: add explicit `default: break;` for Sonar; falls through to `DefWindowProc`. |
| cpp:S5025 | 1 | `icon_cache::hicon_to_png` raw `Gdiplus::Bitmap*` + `delete` → `std::unique_ptr<Gdiplus::Bitmap>`. |
| cpp:S6181 | 1 | `ntquery_snapshot` `std::memcpy(&rec.create_time, &entry->CreateTime, sizeof(FILETIME))` → `rec.create_time = std::bit_cast<FILETIME>(entry->CreateTime);` |

### Accepted (rule not applicable in this codebase)

Apply the comments below when marking "Accept" in SonarCloud UI.

#### `cpp:S3806` — non-portable include case (48)

```
Windows-only project. Windows file system is case-insensitive, so the
case of include paths has no effect on build or runtime. Adopting a
strict lowercase convention would be churn with zero value here.
```

#### `cpp:S5945` — use std::string instead of C-style char array (36)

```
All occurrences are protocol / WinAPI contexts requiring raw char/byte
buffers:
- SOCKS5 / DNS packet wire format (RFC 1928) with fixed byte sequences
- Hot-path receive buffers (uint8_t pkt_buf[65535]) for WinDivert
- WinAPI out parameters (GetModuleFileNameA / QueryFullProcessImageNameW,
  etc.) that require LPSTR/LPWSTR with caller-provided size
- Inline fixed-size fields on hot-path PODs (process_entry::name_u8)
std::string has character semantics, heap allocation, and doesn't
interoperate with these APIs.
```

#### `cpp:S8417` — redundant `std::memory_order::seq_cst` (23)

```
Explicit seq_cst is intentional documentation of required memory
ordering. Removing it would hide deliberate sequential consistency
behind the default and invite unsafe "optimizations" to relaxed without
proper analysis. In concurrent code, explicit intent > brevity.
```

#### `cpp:S1181` — catch-all (`catch (const std::exception&)`) (30)

```
Catching std::exception is the correct pattern here:
- HTTP handlers must return 500 for any failure regardless of type
- Async relay coroutines must not let exceptions escape (triggers
  std::terminate)
- I/O defensive boundaries handle several exception families (JSON,
  filesystem, system) that don't share a more specific common ancestor
Catching multiple concrete types per site would be strictly more code
for no added safety.
```

#### `cpp:S1188` — lambda longer than 20 lines (28)

```
All occurrences are cpp-httplib handler registration lambdas in
http_api_server.hpp. Handler bodies include arg parsing, business logic,
JSON response building, and try/catch — 20-50 lines is the natural size
for this pattern. Splitting each into a named method would add a shim
lambda + method declaration/definition per route (28x) with no real
benefit. Deferred to a future consolidated refactor of http_api_server
(extract handlers per-resource).
```

#### `cpp:S3630` — replace reinterpret_cast with safer operation (21)

```
All occurrences are C ABI interop with no safer alternative:
- Function pointers from GetProcAddress (reinterpret_cast is
  standard-mandated for object↔function pointer conversions)
- Win32 message pump LPARAM/WPARAM/LONG_PTR ↔ struct*/object* (native
  pattern)
- BSD socket sockaddr_in* → sockaddr*, uint8_t* → char* (aliasing-safe
  via char* exception)
- ETW / NtQuery byte buffer → typed fields at dynamic offsets

static_cast cannot bridge these pointer types; std::bit_cast doesn't
apply to pointers; std::memcpy would add a copy on hot paths with no
safety gain since the source is untyped byte memory.
```

#### `cpp:S5827` — replace redundant type with `auto` (12)

```
All occurrences are WinAPI interop sites where the explicit type serves
two purposes beyond "auto":

1. Documentation: BYTE*, wchar_t*, CREATESTRUCT*, DWORD, LONG, ULONG
   are Win32 vocabulary — the name alone tells the reader what kind of
   value this is.

2. Type firewall: if the Windows SDK, WebView2 SDK, or a vcpkg port
   changes a return type in a future version, an explicit declaration
   triggers a compile error and forces review of the change, whereas
   "auto" would silently adopt the new type and potentially mask a
   semantic shift.

Fine to apply S5827 in pure business logic; not here.
```

#### `cpp:S6168` (1 of 11) — detached `std::thread`

```
One-shot fire-and-forget thread (SOCKS5 UDP TCP-control watchdog).
std::jthread here is semantically indistinguishable — detach() discards
its RAII protection — and obscures the deliberate "forget it" intent.
```

#### `cpp:S3574` (14 of 15) — redundant return type on strand_sync lambdas

```
Explicit return type on strand_sync lambdas documents the call's return
type at the call site. Redundant per compiler (auto can deduce), but
preferred here because strand_sync is a template returning
std::invoke_result_t<Lambda> — making it explicit saves the reader from
tracing into the lambda body to know the result type.
```

#### `cpp:S134` (remaining 7) — deep nesting in WndProc / natural iteration idioms

```
Already applied 5 mechanical extractions (see Fixed table). Remaining
sites are:
- webview_app.hpp (WM_NCCALCSIZE / tray menu): Win32 message-pump
  idiom — switch(msg) / case WM_X: + if-else naturally nests 3–4 deep.
  Per-message WndProc split is the canonical refactor (see Open TODOs).
- icon_cache.hpp (snapshot-walk find-by-exe-name): Process32Next loop
  with nested exe-name match → OpenProcess → extract. Natural 3-level.
- config_manager.hpp (JSON v1 → v2 migration): object iteration with
  nested field checks; would fragment into 4+ tiny helpers.
- dns_forwarder.hpp (periodic cleanup of pending_ map):
  modulo-gated timeout sweep; classic iterator-erase pattern.
- flat_tree.hpp (UTF-16 → UTF-8 conversion): Win32 size-probe then
  write pattern; natural 3-level.
Extracting helpers here costs readability; net regression for Sonar
cognitive score was accepted over a forced change.
```

#### `cpp:S5350` (6 of 9) — reference/pointer should be const

```
- ETW event_record_callback self: goes into std::function invocation
  and API-compatible paths; const adds friction without runtime benefit.
- httplib::DataSink::write is non-const (it writes to the sink); cannot
  be called through a const pointer.
- Other sites touch rules_ / cfg methods whose const-ness needs
  per-method verification — not worth the audit for a style warning.
```

#### `cpp:S3608` (10 of 15) — synchronous `[&]` lambdas

```
Synchronous one-shot lambdas (immediate helpers, std::find_if
predicates, visit_descendants callbacks) use [&] idiomatically: the
lambda cannot outlive the enclosing scope, so implicit capture is safe
and keeps the list from drifting when new variables are added to the
enclosing scope. Explicit capture is reserved for stored / async
callbacks.
```

#### `python:*` on `udp_session_research_v2.py` — research playground

```
This file is an isolated UDP / SOCKS5 wire-format research script
deliberately kept as a standalone playground (see win_prox/CLAUDE.md).
Its complexity (python:S3776 × 5), unused return values from socket
reads (python:S1481 × 2), one placeholder-less f-string
(python:S3457 × 1), and one commented-out prior target variant
(python:S125 × 1) all reflect its experimental nature. Accepted rather
than forced to production polish.
```

#### `cpp:S3776` — cognitive complexity (6)

```
All 6 sites overlap with existing architectural refactor TODOs or
are inherent to their function:
- http_api_server::setup_routes (383): entire REST + SSE route
  registration in one function. Planned per-resource handler split
  (Open TODO below) would reduce to ~25 per file.
- main::main (55): startup wiring — engine instantiation, config
  load, HTTP server construction, WebView2 bootstrap, event-loop
  entry. Each path has its own guards and error recovery; splitting
  fragments the startup sequence.
- process/etw_consumer::event_record_callback (29, over by 4):
  ETW event dispatch switch on event ID + inline decoding. Per-event
  helpers would reduce the count but add 5+ small functions for
  marginal gain.
- rule_engine_v3::apply_auto_rules (42): scan entire flat tree x
  rules with hack_tree propagation. Already factored via helpers
  (check_cmdline / check_image_path); remaining complexity is the
  scan matrix itself.
- rule_engine_v3::should_proxy_protocol (26, over by 1): tight
  match + protocol dispatch. Single-digit overage.
- webview_app::WndProc (46): Win32 message pump. Per-message method
  split (Open TODO below) is the canonical refactor.
```

#### Miscellaneous cpp singletons — Accept (1 each)

```
- cpp:S959 (http_api_server.hpp:4) `#undef CPPHTTPLIB_OPENSSL_SUPPORT`:
  intentional strip of upstream vcpkg-defined macro; we don't want
  OpenSSL linkage for cpp-httplib. Standard pattern for controlling
  vendored-lib compile-time options.
- cpp:S1110 (relay.hpp:149) `co_await (pipe(a,b) || pipe(b,a))`:
  parentheses are NOT redundant. `co_await` is unary (high precedence),
  `||` on asio awaitable_operators is logical-or-race (low precedence);
  without the parens the expression becomes `(co_await pipe(a,b)) ||
  pipe(b,a)`, a different program.
- cpp:S1003 (relay.hpp:32) `using namespace asio::experimental::
  awaitable_operators`: required to pick up the overloaded `||` / `&&`
  on awaitables; scoped inside namespace clew, not in a header's top-
  level global scope.
- cpp:S954 (webview_app.hpp:19) `#include <WebView2.h>` inside
  `#ifdef CLEW_HAS_WEBVIEW2`: conditional SDK include is intentional
  (WebView2 SDK optional at build time). Cannot hoist above the ifdef.
- cpp:S5962 / S6168 (socks5_udp_session.hpp:112) detached `std::thread`
  for SOCKS5 UDP TCP-control watchdog: fire-and-forget one-shot
  thread; jthread with detach() has no semantic advantage and obscures
  the intent. Already covered in S6168 partial accept above.
- cpp:S1820 (ntquery_snapshot.hpp:35) 34 fields in `raw_process_record`:
  the struct mirrors SYSTEM_PROCESS_INFORMATION fields extracted from
  NtQuerySystemInformation; splitting by purpose would drift from
  the OS-side layout convention.
- cpp:S859 (etw_consumer.hpp:101) `const_cast<LPWSTR>(SESSION_NAME)`:
  EVENT_TRACE_LOGFILEW::LoggerName is non-const LPWSTR in the Windows
  SDK, but docs confirm the field is read-only on the consumer path.
  const_cast is the documented workaround.
- cpp:S3642 (etw_consumer.hpp:39) `enum` not `enum class`: low-level
  event_id constants used at C-API boundaries (matched against
  EventDescriptor.Id numeric values); enum class would require
  casting at every compare site.
- cpp:S886 (main.cpp:94) argv-parse loop with `argv[++i]`:
  standard CLI idiom for two-part flags (`--static-dir <path>`);
  rewriting with index/while gains nothing but diff.
- cpp:S924 (main.cpp:390) two `break` in init-wait while-loop:
  one break on UI pump failure, one on initialization timeout —
  already distinct reasons, not nested flow.
- cpp:S6194 (dns_forwarder.hpp:122) coroutine cognitive complexity 31:
  DNS query loop (UDP recv → parse → track pending_ → encode → send)
  is a linear pipeline; splitting fragments the coroutine frame state.
  Overlaps with S3776 policy.
- cpp:S6229 (system_dns.hpp:150) `strftime` for dns_state.json
  timestamp: narrow use for a single timestamp field; std::chrono
  formatting is more code with no readability gain.
```

#### `cpp:S2486` (2 of 6) — OOM `catch (const std::bad_alloc&)` bodies in `etw_consumer`

```
cleanup_session / stop_session try/catch guard against allocation
failure for the EVENT_TRACE_PROPERTIES backing buffer. Logging from
the catch body would also allocate (quill format queue), so adding
"handling" is counter-productive. Silent skip is intentional — the
next start() call's cleanup_session orphan-handling re-runs the step
once memory is available.
```

#### `cpp:S2738` — `catch (...)` catch-all (5)

```
All 5 sites require type-agnostic catch by design:
- http_api_server strand_sync (line 193): re-throws via
  std::current_exception across a future — caller gets the exact
  exception type.
- http_api_server group-migration manual_count query (line 1001):
  graceful degradation on strand failure — now logs.
- proxy/relay.hpp (lines 53, 80): coroutine boundary; letting any
  exception escape a coroutine triggers std::terminate.
- socks5_udp_session::watch_tcp_control (line 199): watchdog read;
  any throw means the control connection is lost, which is exactly
  what the alive_ flag toggle handles.
In each case, narrowing to a concrete exception family would miss the
single edge case that matters (e.g., system_error, bad_cast,
protocol-specific exceptions) and produce strictly worse behavior.
```

#### `typescript:S7764` (1 of 3) — `window.chrome` in `App.vue:43`

```
The `chrome.webview` bridge property is declared via
`declare global { interface Window { chrome?: {...} } }` augmentation.
globalThis uses its own type (typeof globalThis), so the augmentation
on Window does not propagate. Accessing `globalThis.chrome` would
require a parallel globalThis augmentation or a cast; keeping
`window.chrome` keeps the type chain honest.
```

#### `cpp:S6190` — replace macro with `std::source_location` (2)

```
ASSERT_TRUE / ASSERT_EQ macros use `#expr` stringification to embed
the asserted expression in the failure message — a property that
std::source_location cannot provide. A hybrid wrapper (thin macro →
function with source_location) would still need the outer macro for
stringify, with no real reduction in surface area. The current pattern
is idiomatic across C++ test harnesses (Catch2/doctest/GTest all use
macros). Not worth churn for a 30-line built-in harness.
```

#### `cpp:S125` — commented-out code (2)

```
False positives: both sites are descriptive comments, not dead code.
- config_manager.hpp:180 `// === v2 API ===` is a section divider
- socks5_udp_codec.hpp:47 `// DST.PORT: big-endian (network byte order)`
  is a SOCKS5 protocol field description
Sonar's "looks like code" heuristic misfires on `===` and protocol
field names.
```

#### `cpp:S6045` — transparent equality / heterogeneous hash (3)

```
Sites are:
- icon_cache::cache_ / name_cache_ — all lookups go through
  normalize_key(), which unconditionally allocates a new std::string
  to lowercase the path. Transparent lookup yields no savings because
  the key is already std::string by construction.
- config_manager::rules — admin-frequency operations only (create/
  update/delete rules); not a hot path. Real callers pass std::string.
Introducing a transparent hasher (is_transparent + const char*/
string_view overloads) adds ~20 LOC of boilerplate across the file
for no observable perf gain.
```

#### `cpp:S7040` — delimited escape sequence `\x{1}` (4)

```
C++23 delimited escape sequences (P2290, `\x{1}` form) are not yet
implemented in MSVC as of 19.40. Compilation fails with C2153 "integer
literal must have at least one digit" because MSVC still parses `\x`
as the legacy form and treats `{` as a following character. Existing
`\x01` is unambiguous here (nothing follows in the string or char
literal). Revisit when MSVC catches up.
```

#### `cpp:S5421` — global variables should be const (5)

```
All 5 globals are legitimately mutable shared state:
- main.cpp g_running: std::atomic<bool> toggled by signal_handler to
  stop the main loop (textbook signal handler pattern).
- tests/test_components.cpp g_pass / g_fail / g_errors / g_tests:
  accumulated by the minimal test framework during execution
  (pass/fail counters, error list, static test registration via
  pre-main constructors).
Making any of these const would break their purpose. Sonar does not
appear to track the writes through signal_handler and the
pre-main registration lambda.
```

#### `cpp:S6022` — use `std::byte` for byte-oriented data (7)

```
All occurrences are idiomatic `uint8_t` uses at system/network boundaries:
- DNS wire parsing (frame->data[n] bit-ops to assemble uint16_t rcode/ancount)
- SOCKS5 UDP codec (reinterpret_cast const uint8_t* to extract 4 IP bytes)
- Win32 NT structure walking (reinterpret_cast<BYTE*>(entry) + offset)
- Flat tree flags bitfield (flags |= f, flags & f, flags &= ~f)
Switching to std::byte would require std::to_integer<uint8_t>() or
std::byte{f} wrapping at every access point. The source is always raw
OS/network bytes; std::byte adds verbosity with no type-safety gain here.
```

#### `cpp:S6391` (1 of 2) — coroutine parameter by value (`relay.hpp:73` `groups`)

```
handle_connection takes `const std::unordered_map<uint32_t,
ProxyGroupConfig>& groups`. The reference comes from async_acceptor's
`groups_` member, which is a reference to the engine's group map owned
by main.cpp — program-lifetime. Inside the coroutine, the map is only
accessed within a strand-bound inner co_spawn (serialized read, result
copied into a local `GroupInfo`), and never touched after that point.
Passing the full map by value per accepted connection would add a
non-trivial copy on every hijacked connection with no real safety gain,
since the design invariant (strand-serialized access, long-lived owner)
already prevents dangling.
```

#### `cpp:S6004` — init-statement in `if` (30)

```
Style-only: if (auto X = find(k); X != end) ... is technically tighter,
but many sites would get awkward line-wraps. Kept the pre-C++17 form
for consistency across hot-path code.
```

---

## Open TODOs

- **`std::string_view` migration (remaining hot-path)** — `wildcard_match` /
  `check_cmdline` / `check_image_path` signatures still take `const
  std::string&`, which causes implicit `std::string` construction from
  `entry.name_u8` (`char[780]`) on every match attempt. Not yet flagged by
  Sonar (signatures internal), but still worth doing as an independent
  zero-cost refactor.
- **`http_api_server.hpp` consolidated refactor** — extract handlers
  per-resource (e.g. `handlers/process.hpp`, `handlers/rule.hpp`). Would
  naturally resolve `cpp:S1188` and part of `cpp:S134`.
- **WndProc per-message methods** in `webview_app.hpp` — resolves the
  remaining `cpp:S134` sites in Win32 message pump.
