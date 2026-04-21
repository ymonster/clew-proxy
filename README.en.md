# Clew

> Windows process-level traffic proxy

**Languages**: [简体中文](README.md) · [English](README.en.md)

---

## Overview

Clew is a process-level transparent proxy for Windows. In spirit, it's like Proxifier or SocksCap. The original motivation was that antigravity without TUN mode had various issues, and with TUN mode on it broke other apps — I didn't like hijacking traffic globally, so I built this.

- **Based on WinDivert**: building our own WFP driver needs code signing (annual fee), not worth it for a personal open-source project
- **Process-tree proxying**: once a rule matches, the entire child-process tree (including dynamically spawned children) is proxied together
- **Command-line matching**: when multiple processes share the same executable name (e.g. several `python.exe` running different scripts), match by cmdline keywords to target only the one you want
- **Multiple SOCKS5 backends**: configure several proxy groups on one machine, route different rules to different groups
- **UDP support**: per-app-port SOCKS5 UDP ASSOCIATE, strictly RFC 1928 compliant
- **Built-in DNS forwarder**: auto-configures system DNS to a local forwarder that sends queries through SOCKS5 UDP to upstream resolvers (8.8.8.8, etc.), solving DNS pollution and geo-mismatch issues
- **WebView2 native UI**: Vue 3 + shadcn-vue, no browser required

## Install & Run

### Requirements

- Windows 10 (version 2004+) or Windows 11
- Administrator privileges (required for both WinDivert driver and system DNS configuration)

### Download

Grab the latest release from [Releases](https://github.com/ymonster/clew-proxy/releases). You may need to install the VS2022 redistributable (available from Microsoft's site).

## Usage

### 1. Configure proxy backend

Settings → Proxies. A `default` group exists; change host / port to point at your SOCKS5 proxy (e.g., Clash / Shadowsocks at `127.0.0.1:7890`).

### 2. Add an Auto Rule

Auto Rules tab → New:

- **Process name**: glob wildcards supported, e.g., `curl*`, `python.exe`
- **Cmdline pattern**: keywords that must appear in the command line (space-separated, case-insensitive, order-independent)
- **Hack tree**: on = also proxy all child processes
- **Protocol**: TCP / UDP / Both
- **Proxy group**: which proxy group to route through

Save → launching the matching process triggers the proxy.

### 3. Manually proxy a process

In the **Process Tree**, hovering over a process reveals a small icon on the right; click it to proxy.

### 4. DNS Proxy

Some apps (Spotify, certain geo-sensitive services) have trouble with DNS resolution. Enable Settings → DNS Proxy:

- **Forwarder mode**: auto-reconfigures system DNS to `127.0.0.2` (Clew's built-in forwarder). All DNS queries go through SOCKS5 → upstream.
- On disable / exit, the system DNS is auto-restored. Even if the process is force-killed, the next launch detects and restores the original configuration.

### 5. API token auth (optional)

The HTTP API listens on `127.0.0.1:18080` with auth **disabled** by default. If you're worried about other local processes hitting destructive endpoints like `/api/hijack`, you can enable it:

Edit `clew.json`:

```json
"auth": {
  "enabled": true,
  "token": "a-sufficiently-random-string"
}
```

After restarting Clew:

- **WebView2 UI**: a one-time prompt asks for the token on first load; `localStorage` remembers it afterwards
- **curl / scripts**: attach `Authorization: Bearer a-sufficiently-random-string`; SSE doesn't support custom headers, use the query-string fallback `?token=...`

### Why WinDivert

- WinDivert is a mature WFP callout driver with stable Win10/11 support, open source, and actively maintained
- **LGPL v3** licensing lets us dynamically link the `.dll` without forcing our main project license
- Its dual-layer architecture (SOCKET SNIFF + NETWORK reflection) records the `PID → group` mapping at connect time, so the redirection path is zero-lookup
- **WinDivert is free, our code is open source — zero cost**

### Why no Hook mode (DLL injection)

I tried a DLL injection + `GetAddrInfoW` hook path, but two issues came up:

1. **Chromium / Electron apps use their own DNS resolver**: modern Chromium increasingly bypasses `GetAddrInfoW` in favor of an async UDP DNS client. Hooking just `GetAddrInfoW` misses those (Spotify worked in testing, but changing the proxy backend's own DNS config achieves the same thing).
2. **Full-stack hooking is expensive**: reliable proxying needs `getaddrinfo` / `GetAddrInfoW` / `connect` / `WSAConnect` / `ConnectEx` hooked simultaneously, plus dealing with sandbox policies (`ProcessSignaturePolicy`), signature enforcement, anti-injection, etc. (Signing isn't free either!)

### v2: things I might do

1. If enough people actually need it, I might do an in-house WFP callout driver paired with `PsSetCreateProcessNotifyRoutineEx` — more thorough than today, wouldn't miss any event. The blocker is the annual signing cost.

   For my personal use, what we have is enough. Full-stack hooking remains an option too.

2. Application-layer protocol identification (http/quic/...)
3. LUA plugin support, so users can mutate upstream/downstream traffic for specific processes and protocols

## Build

### Prerequisites

- Visual Studio 2022
- CMake 3.20+
- vcpkg (`quill`, `nlohmann-json`, `cpp-httplib`, `asio`)
- Node.js 20+ (frontend build)
- WebView2 Runtime (bundled on Win11; download from Microsoft on Win10)

### Steps

```bash
# 1. Configure
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 --preset windows-vcpkg

# 2. Build backend
cmake --build build --config Release

# 3. Build frontend
cd frontend
npm install
npm run build
cd ..

# 4. Run
./build/Release/clew.exe
```

## License

- Clew itself: [MIT](LICENSE)
- [WinDivert](https://github.com/basil00/Divert) dependency: LGPL v3 (dynamically linked; original license kept at `WinDivert-2.2.2-A/LICENSE`)
