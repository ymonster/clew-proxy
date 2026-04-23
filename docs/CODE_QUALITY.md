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
| cpp:S5350 (partial) | 3 | Add `const` to read-only reference/pointer variables in `flat_tree.hpp` |
| cpp:S3574 (partial) | 1 | Remove redundant `-> char` on single-line lambda |

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

#### `cpp:S134` (11 of 16) — deep nesting in Win32 WndProc / edge cases

```
Win32 message-pump idiom: switch(msg) / case WM_X: + if-else forms
naturally nest 3–4 deep. Refactoring to "one method per message"
(on_size / on_close / ...) is a dedicated WndProc-wide rewrite; not
worth doing case-by-case.
Remaining sites nest exactly 3 deep with straightforward control flow —
extracting a helper would hurt readability more than the nesting does.
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

#### `cpp:S6004` — init-statement in `if` (30)

```
Style-only: if (auto X = find(k); X != end) ... is technically tighter,
but many sites would get awkward line-wraps. Kept the pre-C++17 form
for consistency across hot-path code.
```

---

## Open TODOs

- **`std::string_view` migration** for `wildcard_match` / `check_cmdline`
  / `check_image_path` signatures — eliminates implicit `std::string`
  construction from `entry.name_u8` (`char[780]`). Zero-cost refactor,
  independent of any current fix.
- **`http_api_server.hpp` consolidated refactor** — extract handlers
  per-resource (e.g. `handlers/process.hpp`, `handlers/rule.hpp`). Would
  naturally resolve `cpp:S1188` and part of `cpp:S134`.
- **WndProc per-message methods** in `webview_app.hpp` — resolves the
  remaining `cpp:S134` sites in Win32 message pump.
