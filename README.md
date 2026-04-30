# Clew

> Windows 进程级流量代理

**Languages**: [简体中文](README.md) · [English](README.en.md)

[![Release](https://img.shields.io/github/v/release/ymonster/clew-proxy)](https://github.com/ymonster/clew-proxy/releases)
[![License](https://img.shields.io/github/license/ymonster/clew-proxy)](LICENSE)

![Main UI](docs/images/main_ui.png)

---

## 简介

一个跑在 Windows 上的进程级流量代理工具。

做它的起因是很早前用 Antigravity 时发现：后端 Clash 不开 TUN mode 甚至无法刷新模型列表，开了又影响其它应用。我不想为了一个软件全局劫持流量，就做了个工具让特定进程独立走代理，一直到现在这个版本。

## 特性

- **进程树代理**：一条规则命中后，它的所有子进程（包括启动后才动态派生的）会自动跟着走代理。Chromium / Electron 类应用会派生很多子进程，只代理主进程通常不够，或者你需要做足够的研究才能知道究竟需要代理哪个进程。那么现在你可以在 `Rules` 页面去增加规则，选择特定 image path 的 exe 文件，也可以只输入一个名字（比如 Antigravity.exe），都可以完成任务。
- **命令行级别匹配**：比如同一个 `python.exe` 跑不同脚本时， `Rules` 页面配置你的规则时，可以用命令行关键词进行匹配（比如只代理跑 `crawler.py` 的那个进程，不影响其它 python 进程）。
- **多 SOCKS5 后端**：支持多组代理，不同规则路由到不同组。因为我用到的大多数代理服务工具都支持 socks5，而 http 代理这些工具自身就支持，不需要我来做。
- **UDP 支持**：per-app-port SOCKS5 UDP ASSOCIATE，基于 RFC 1928。
- **内置 DNS forwarder**：启用后把系统 DNS 自动指向 Clew 的内置 forwarder，DNS 查询走 SOCKS5 到上游；关闭或退出会还原系统 DNS，异常被强杀，下次启动也会检测残留并恢复。
- **UI 即时响应**：v0.8.1 及之前 hack / unhack 等操作偶尔会卡几秒；这版分开修了两个互不相关的成因：
  - **状态推送改走 WebView2 进程内 IPC**：原先用 HTTP/SSE 推送，SSE 长连接会占住 HTTP/1.1 同源 6 连接里的一个槽位，并且 chunked 字节流的解码常驻 V8 主线程；换成 `PostWebMessageAsJson` 后推送不再走浏览器网络栈，连接池和 V8 都松快了。
  - **DELETE 请求显式带 `body: ''`**：cpp-httplib 把所有 DELETE 当作"可能有 body"，没看到 Content-Length 时会 peek 等满 5 秒 `read_timeout` 才把请求交给 handler；浏览器 fetch DELETE 默认不发 Content-Length，结果一次 unhack 死等 5 秒。前端在 DELETE 请求里显式带空 body 让浏览器写出 `Content-Length: 0`，cpp-httplib 走快路立即处理。

## 这个工具适不适合我

**适合**

- 你想让某个app走代理，但它可能不只有http访问，也有tcp/udp访问，同时你完全不知道或者不想知道到底是哪个子进程做了网络活动。
- 已经有 Clash 这类代理服务客户端在跑，只想让几个特定应用走它，不想开全局 TUN。
- 在意 MIT 许可证，要集成进内部工具链或商业产品而不沾 AGPL 传染风险。

**不适合**

- 需要 macOS / Linux → 看 [ProxyBridge](https://github.com/InterceptSuite/ProxyBridge)
- 只要命令行不要 GUI → 看 [ProxiFyre](https://github.com/wiresock/proxifyre)

## 安装与运行

### 系统要求

- Windows 10 版本 2004 或更新 / Windows 11
- 管理员权限（WinDivert 驱动和系统 DNS 配置都需要）
- [VS2022 C++ 运行时](https://aka.ms/vs/17/release/vc_redist.x64.exe)
- WebView2 Runtime（Windows 11 自带；Windows 10 从 [微软](https://developer.microsoft.com/microsoft-edge/webview2/) 下载）

### 下载

[Releases 页](https://github.com/ymonster/clew-proxy/releases) 下载最新 zip，解压，双击 `clew.exe`，会自动弹 UAC 提权。

## 使用

### 1. 配置代理后端

Proxies 标签页

- 默认已经有一个 `default` 组，改 host / port 指向你的 SOCKS5（比如 Clash 的 `127.0.0.1:7890`）。
- 点击刷新图标可以进行延迟测试

![Proxy group](docs/images/proxy_group.png)

### 2. 添加 Auto Rule

Rules 标签页

- 点击 `Add Rule` 可以新增规则，新建规则默认 OFF，要手动打开开关才会生效
  - **Name**：规则名
  - **Process name**：通配符，例如 `curl*`、`python.exe`
  - **Cmdline pattern**：命令行关键词（多关键词空格分隔，大小写不敏感，顺序无关）
  - **Hack tree**：勾上 = 子进程也跟着代理
  - **Protocol**：TCP / UDP / Both
  - **Proxy group**：走哪个代理组
- 可以查看当前的规则列表和状态

![Rules](docs/images/rules.png)

### 3. 进程树和 Network Activities

- **Process Tree**：进程树。hover 某个进程，右侧会出现一个小闪电图标，点击即可代理当前这个 PID（含它未来派生的子进程）。适合临时调试。**手动 hack 默认同时代理 TCP + UDP，不可选协议**；如果只想代理单一协议，请用 Auto Rule 的 `Protocol` 字段。

  ![Process tree hover](docs/images/process_hover.png)

- **All Processes**：点击左侧 `All Processes`（或不选中任何特定进程）可以看到所有进程的实时网络活动。
- **Network Activities 进程头**：选中某个进程后，上方会出现这个进程的详细信息条。
  - **定位**：进程名左侧 explorer 图标可以定位进程所在目录
  - **复制信息**：路径栏 hover 会出现两个复制图标，左侧复制的是 working directory，右侧 `all` 复制连同 image path 在内的完整 cmdline
  - **从进程增加规则**：右侧 `+` 图标可以以此进程为蓝本新增一条规则

  ![Network Activities process header](docs/images/network_process_hover_to_copy.png)

### 4. DNS Proxy（默认关闭）

部分应用（Spotify、某些海外服务）对 DNS 解析的地域敏感。Settings → DNS Proxy 打开后：

- 把系统 DNS 自动配置成 127.0.0.2（Clew 内置 forwarder）
- DNS 查询走 SOCKS5 到上游（默认 8.8.8.8）
- 关闭 / 退出自动还原系统 DNS；异常退出下次启动会检测残留并恢复

## 技术选择

### 第三方库

后端：

- [WinDivert](https://github.com/basil00/Divert) — Windows 下基于 WFP 的用户态抓包 / 转发库；动态链接，随 release 一起分发（LGPL v3）
- [quill](https://github.com/odygrd/quill) — 异步结构化日志
- [nlohmann/json](https://github.com/nlohmann/json) — JSON 解析和序列化
- [cpp-httplib](https://github.com/yhirose/cpp-httplib) — 内置 HTTP + SSE 服务端
- [asio](https://think-async.com/Asio/) — 异步 IO
- [WebView2](https://developer.microsoft.com/microsoft-edge/webview2/) — 嵌入 Edge 渲染前端 UI

前端：

- [Vue 3](https://vuejs.org/) + TypeScript + [Vite](https://vitejs.dev/) — 应用框架和构建
- [reka-ui](https://reka-ui.com/)（shadcn-vue 风格） + [Tailwind CSS](https://tailwindcss.com/) — 组件 + 样式
- [AG Grid Community](https://www.ag-grid.com/) — Network Activities 表格
- [Monaco Editor](https://microsoft.github.io/monaco-editor/) — Settings 里的 JSON 编辑器（懒加载）
- [Lucide](https://lucide.dev/) — 图标

完整许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

### 为什么用 WinDivert

- 成熟的 WFP callout driver，Win10/11 稳定支持
- LGPL v3 允许动态链接 `.dll`，主项目可以保持 MIT
- **免费**：WinDivert 作者已经付了 kernel driver 签名费用，发布的就是签好的 .sys

### 为什么现在不用 DLL 注入路线

1. 尝试过注入 `GetAddrInfoW` 效果不错，但现在有很多自带异步 DNS resolver 的进程无法覆盖。所以需要同时注入 `getaddrinfo` / `GetAddrInfoW` / `connect` / `WSAConnect` / `ConnectEx` 等网络相关调用，工程量稍大。
2. 使用 `PsSetCreateProcessNotifyRoutineEx` 可以更准确地发现进程的创建和销毁，结合网络层能更准确地拦截网络操作，注入等动作会更准确和高效。
3. 但以上技术在我本机测试中可行，实际环境中需要考虑安全软件，需要通过 SignPath 这类服务签名，以及适配各种安全软件等一系列操作，本身周期就很长，甚至可能需要付费。
4. 结论：WinDivert 目前够用，但如果有相应需求可以单开 branch 尝试 DLL 注入。

### 为什么不做自研 WFP kernel driver

技术上更完善（`FWPM_LAYER_ALE_CONNECT_REDIRECT_V4` 直接改 socket 目标、零包操作），但有一个非常关键的问题： Windows 10+ 要求 kernel driver 必须由 Microsoft 签名，走 Partner Center 需要一张 EV code signing 证书（约 300–600 美元 / 年）和 attestation 流程，成本过高。

### 进程树更新策略（`CLEW_PROJECTION_COALESCE`）

默认每个 ETW 进程事件直接刷新前端、即时推送，不做 timer 合并。常规使用——即使 1000+ 进程在跑——完全没问题，strand 利用率不到 1%。只有**瞬间 +1000 进程的 ETW 风暴**（比如某个工具一次 fork 出几千个子进程）才可能让 strand 短时间被序列化负担占住；UI 上常用的"看某个特定进程"这类操作即使在风暴期间也不会被影响。如果遇到这类极端工作负载且需要进一步缓解，可以编译时打开 `cmake -DCLEW_PROJECTION_COALESCE=ON` 启用 100ms 合并窗口（代价：树更新最多滞后 100ms）。

## 构建

### 构建工具链

- Visual Studio 2022（C++23）
- CMake 3.20+
- vcpkg
- Node.js 20+
- WebView2 SDK

### 步骤

```bash
# 配置
cmake --preset windows-vcpkg

# 后端（refactor 之后 TU 数量从 2 个涨到 ~30 个，首次 clean build 会比 legacy 版本慢一些；
# CMakeLists 已开启 /MP 并行编译，多核机器明显缓解）
cmake --build build --config Release

# 前端
cd frontend
npm install
npm run build
cd ..

# 运行
./build/Release/clew.exe
```

## 贡献

欢迎 Issue / PR。项目结构、架构说明、API schema 见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## License

- Clew：[MIT](LICENSE)
- 动态链接的 [WinDivert](https://github.com/basil00/Divert)：LGPL v3（原许可证保留于 `WinDivert-2.2.2-A/LICENSE`）
- 其它第三方依赖许可：见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
