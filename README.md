# Clew

> Windows 进程级流量代理

**Languages**: [简体中文](README.md) · [English](README.en.md)

---

## 简介

Clew 是一个运行在 Windows 的进程级流量代理工具。和 Proxifier / SocksCap 类似。最初做这个是因为早先 antigravity 不开 tun mode 就会有各种问题，开了也会导致其他应用的问题。而我很讨厌直接就全局劫持流量，所以做了这个小玩意儿。

- **基于 WinDivert**：自研 WFP 驱动需要代码签名（年费），对个人开源项目不划算
- **进程树代理**：一次命中规则，整棵子进程树（含动态创建的子进程）自动一并代理
- **命令行级别匹配**：同一个 `python.exe` 跑不同脚本时，用命令行关键词精确锁定目标（比如只代理跑 `crawler.py` 的那个进程，不影响其它 python 进程）
- **多 SOCKS5 后端**：同一机器上可配置多组代理，不同规则路由到不同组
- **UDP 支持**：per-app-port SOCKS5 UDP ASSOCIATE，严格遵循 RFC 1928
- **内置 DNS forwarder**：自动把系统 DNS 指向内置 forwarder → SOCKS5 UDP → 上游（8.8.8.8 等），解决 DNS 污染 / 地域不一致问题
- **WebView2 前端**：Vue 3 + shadcn-vue 实现的原生桌面 UI，无需浏览器

## 安装与运行

### 要求

- Windows 10 (版本 2004 或更新) / Windows 11
- 管理员权限（WinDivert 驱动和系统 DNS 配置都需要）

### 下载

从 [Releases](https://github.com/ymonster/clew-proxy/releases) 页下载，可能需要安装 VS2022 的 redist，微软官网就有。

## 使用指南

### 1. 配置代理后端

Settings → Proxies，默认已经有一个 `default` 组，修改 host / port 指向你的 SOCKS5 代理（比如 Clash / Shadowsocks 的 127.0.0.1:7890）。

### 2. 添加 Auto Rule

在 **Auto Rules** 标签页点 "New"：

- **Process name**：支持通配符，比如 `curl*`、`python.exe`、`*.exe`
- **Cmdline pattern**：命令行包含什么关键词（多个空格分隔、大小写不敏感、顺序无关）
- **Hack tree**：勾上 = 连同所有子进程一起代理
- **Protocol**：TCP / UDP / Both
- **Proxy group**：走哪个代理组

保存 → 启动对应进程即被代理。

### 3. 手动代理某个进程

**Process Tree** 里 hover 进程会在右侧出现小图标，点击即可代理。

### 4. DNS Proxy

部分应用（Spotify、某些海外服务）对地域敏感的 DNS 解析会出问题。开启 Settings → DNS Proxy：

- **Forwarder 模式**：自动把系统 DNS 配置成 127.0.0.2（Clew 内置 forwarder），所有 DNS 查询走 SOCKS5 → 上游。
- 关闭 / 退出时自动还原系统 DNS；即使进程被强杀，下次启动也会自动检测并恢复。

### 5. API Token 认证（可选）

HTTP API 监听在 `127.0.0.1:18080`，默认**关闭**鉴权。如果担心同机其他进程调 `/api/hijack` 等破坏性接口，可以打开：

编辑 `clew.json`：

```json
"auth": {
  "enabled": true,
  "token": "一串足够随机的字符"
}
```

重启 Clew 后：

- **WebView2 前端**：首次打开会弹一个对话框让你填一次 token，浏览器 localStorage 记住，之后透明使用
- **curl / 脚本访问**：请求头带 `Authorization: Bearer 一串足够随机的字符`；SSE 不支持自定义 header，用查询参数 `?token=...`

### 为什么选 WinDivert

- WinDivert 是一个成熟的 WFP callout driver，稳定支持 Win10/11，开源且社区活跃
- **LGPL v3** 授权允许我们动态链接 `.dll` 而不污染主项目许可证
- 双层架构（SOCKET SNIFF + NETWORK reflection）可以在 socket 建立阶段就记录 PID→group 映射，后续重定向零查表
- **WinDivert 本体免费，我们自己开源，零成本**

### 为什么没做 Hook mode（DLL 注入）

我试过 DLL 注入 `GetAddrInfoW` 的路线，但有两个问题：

1. **Chromium / Electron 应用用自建 DNS resolver**：新版 Chromium 越来越多地绕开 `GetAddrInfoW`，直接走 async UDP DNS 客户端。只 hook `GetAddrInfoW` 覆盖不到（实测 Spotify 能覆盖，但改后端代理的 DNS 配置也能达到同样效果）。
2. **全链路 hook 成本高**：要可靠代理一个应用，得同时 hook `getaddrinfo`/`GetAddrInfoW`/`connect`/`WSAConnect`/`ConnectEx`；还要应对沙箱策略（`ProcessSignaturePolicy`）、签名校验、反注入等各类拦截。（签名要收费！）

### v2：未来可能会做的

1. 如果真有很多人有这个需求，可能会考虑使用 WFP callout driver 技术，配合 `PsSetCreateProcessNotifyRoutineEx` 能比现在更完善，不会错过任何信息。但主要是这个路线的签名什么的费用太高了。

   我觉得对我目前的需求来说够用了，当然全链路 hook 也是一个选项。

2. 加入对应用层协议的基本标识（http/quic...）
3. 加入支持 LUA 的插件能力，可以针对特定进程特定协议上下行做修改

## 构建

### 依赖

- Visual Studio 2022
- CMake 3.20+
- vcpkg（`quill`, `nlohmann-json`, `cpp-httplib`, `asio`）
- Node.js 20+（前端构建）
- WebView2 Runtime（Windows 11 自带；Windows 10 从微软网站安装）

### 步骤

```bash
# 1. 配置
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 --preset windows-vcpkg

# 2. 构建后端
cmake --build build --config Release

# 3. 构建前端
cd frontend
npm install
npm run build
cd ..

# 4. 运行
./build/Release/clew.exe
```

## License

- Clew 本体：[MIT](LICENSE)
- 依赖的 [WinDivert](https://github.com/basil00/Divert)：LGPL v3（动态链接，我们保留其原许可证于 `WinDivert-2.2.2-A/LICENSE`）
