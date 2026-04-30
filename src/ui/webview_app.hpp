#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>
#ifdef CLEW_HAS_WEBVIEW2
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#endif
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include "core/log.hpp"
#include "transport/frontend_push_sink.hpp"

#pragma comment(lib, "dwmapi.lib")

namespace clew {

constexpr UINT WM_TRAYICON         = WM_APP + 1;
constexpr UINT WM_PUSH_TO_FRONTEND = WM_APP + 2;
constexpr UINT IDM_TRAY_SHOW = 40001;
constexpr UINT IDM_TRAY_EXIT = 40002;

class webview_app : public frontend_push_sink {
    // Heap-allocated payload smuggled through PostMessageW. Producer thread
    // releases ownership into LPARAM; UI thread takes it back via unique_ptr
    // in the WM_PUSH_TO_FRONTEND case.
    struct push_payload {
        std::string event;
        std::string json_body;
    };
private:

    HWND hwnd_ = nullptr;
    std::wstring url_;
    int width_ = 1200;
    int height_ = 800;
    int init_x_ = -1;  // -1 = center
    int init_y_ = -1;
    std::wstring title_ = L"Clew";
    std::function<void()> on_close_;
    std::function<void(int, int, int, int)> on_move_resize_;
    bool initialized_ = false;
    bool close_to_tray_ = false;
    bool devtools_enabled_ = true;
    NOTIFYICONDATAW nid_ = {};
    bool tray_created_ = false;
    std::function<void()> on_ready_;

#ifdef CLEW_HAS_WEBVIEW2
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> webview_controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;

    void initialize_webview() {
        wchar_t exe_path[MAX_PATH];
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::wstring user_data_folder = exe_path;
        size_t last_slash = user_data_folder.find_last_of(L"\\/");
        if (last_slash != std::wstring::npos) {
            user_data_folder = user_data_folder.substr(0, last_slash) + L"\\clew_webview_data";
        }

        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        // WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS env var is normally honored
        // by WebView2, but only when the host did NOT call
        // put_AdditionalBrowserArguments. Since we always pass --no-proxy-server,
        // we'd otherwise mask the env var. Merge it explicitly so testers can
        // do e.g. WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=9222
        // to attach Playwright over CDP.
        std::wstring browser_args = L"--no-proxy-server";
        if (auto* env = std::getenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"); env && *env) {
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, env, -1, nullptr, 0);
            if (wlen > 1) {
                std::wstring extra(static_cast<size_t>(wlen - 1), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, env, -1, extra.data(), wlen);
                browser_args.push_back(L' ');
                browser_args.append(extra);
            }
        }
        options->put_AdditionalBrowserArguments(browser_args.c_str());

        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, user_data_folder.c_str(), options.Get(),
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(result)) {
                        PC_LOG_ERROR("Failed to create WebView2 environment: {:#x}", static_cast<unsigned int>(result));
                        return result;
                    }
                    return env->CreateCoreWebView2Controller(
                        hwnd_,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(result) || !controller) {
                                    PC_LOG_ERROR("Failed to create WebView2 controller: {:#x}", static_cast<unsigned int>(result));
                                    return result;
                                }
                                webview_controller_ = controller;

                                // Set dark background color to prevent white flash
                                Microsoft::WRL::ComPtr<ICoreWebView2Controller2> controller2;
                                if (SUCCEEDED(webview_controller_->QueryInterface(IID_PPV_ARGS(&controller2)))) {
                                    COREWEBVIEW2_COLOR bg = {255, 9, 9, 11}; // #09090b
                                    controller2->put_DefaultBackgroundColor(bg);
                                }

                                webview_controller_->get_CoreWebView2(&webview_);
                                RECT bounds;
                                GetClientRect(hwnd_, &bounds);
                                webview_controller_->put_Bounds(bounds);

                                // Settings
                                ICoreWebView2Settings* settings = nullptr;
                                webview_->get_Settings(&settings);
                                if (settings) {
                                    settings->put_IsScriptEnabled(TRUE);
                                    settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                                    settings->put_IsWebMessageEnabled(TRUE);
                                    settings->put_AreDevToolsEnabled(devtools_enabled_ ? TRUE : FALSE);
                                    // Kill the default browser context menu (Print / View source /
                                    // Save image / Reload / etc.) — meaningless in a desktop UI.
                                    // Ctrl+C/V/X still work; F12 still opens DevTools when enabled.
                                    settings->put_AreDefaultContextMenusEnabled(FALSE);

                                    // Enable non-client region support for CSS app-region: drag
                                    Microsoft::WRL::ComPtr<ICoreWebView2Settings9> settings9;
                                    if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings9)))) {
                                        settings9->put_IsNonClientRegionSupportEnabled(TRUE);
                                        PC_LOG_INFO("WebView2 NonClientRegion support enabled");
                                    }

                                    settings->Release();
                                }

                                // Web message handler — dispatches both:
                                //   1) plain-string window-control commands
                                //      ("minimize" / "maximize" / "close")
                                //   2) structured JSON messages
                                //      ({"type":"ready", ...})
                                webview_->add_WebMessageReceived(
                                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            // Plain-string path first.
                                            LPWSTR str_msg = nullptr;
                                            if (SUCCEEDED(args->TryGetWebMessageAsString(&str_msg)) && str_msg) {
                                                std::wstring cmd(str_msg);
                                                CoTaskMemFree(str_msg);
                                                if (cmd == L"minimize") {
                                                    ShowWindow(hwnd_, SW_MINIMIZE);
                                                } else if (cmd == L"maximize") {
                                                    ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
                                                } else if (cmd == L"close") {
                                                    SendMessage(hwnd_, WM_CLOSE, 0, 0);
                                                }
                                                return S_OK;
                                            }

                                            // Structured JSON path. We only care about a tiny set of
                                            // fields right now, so a substring check is sufficient
                                            // and avoids pulling nlohmann::json into this header.
                                            LPWSTR json_msg = nullptr;
                                            if (SUCCEEDED(args->get_WebMessageAsJson(&json_msg)) && json_msg) {
                                                std::wstring js(json_msg);
                                                CoTaskMemFree(json_msg);
                                                if (js.find(L"\"type\":\"ready\"") != std::wstring::npos
                                                    || js.find(L"\"type\": \"ready\"") != std::wstring::npos) {
                                                    PC_LOG_INFO("[push] frontend ready handshake received");
                                                    if (on_ready_) on_ready_();
                                                }
                                            }
                                            return S_OK;
                                        }
                                    ).Get(), nullptr);

                                webview_->Navigate(url_.c_str());
                                {
                                    int len = WideCharToMultiByte(CP_UTF8, 0, url_.c_str(), -1, nullptr, 0, nullptr, nullptr);
                                    std::string url_utf8(len - 1, 0);
                                    WideCharToMultiByte(CP_UTF8, 0, url_.c_str(), -1, url_utf8.data(), len, nullptr, nullptr);
                                    PC_LOG_INFO("WebView2 initialized, navigating to {}", url_utf8);
                                }
                                initialized_ = true;
                                return S_OK;
                            }
                        ).Get()
                    );
                }
            ).Get()
        );

        if (FAILED(hr)) {
            PC_LOG_ERROR("CreateCoreWebView2EnvironmentWithOptions failed: {:#x}", static_cast<unsigned int>(hr));
        }
    }

    void resize_webview() {
        if (webview_controller_) {
            RECT bounds;
            GetClientRect(hwnd_, &bounds);
            webview_controller_->put_Bounds(bounds);
        }
    }
#else
    void initialize_webview() {
        PC_LOG_WARN("WebView2 support not compiled in (CLEW_HAS_WEBVIEW2 not defined)");
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, url_.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string url_narrow(size_needed - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, url_.c_str(), -1, url_narrow.data(), size_needed, nullptr, nullptr);
        PC_LOG_INFO("Please access the UI at: {}", url_narrow);
    }

    void resize_webview() {}
#endif

    void create_tray_icon() {
        if (tray_created_) return;
        nid_.cbSize = sizeof(NOTIFYICONDATAW);
        nid_.hWnd = hwnd_;
        nid_.uID = 1;
        nid_.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        nid_.uCallbackMessage = WM_TRAYICON;
        // Load the embedded Clew icon (resource ID 1 from assets/clew.rc).
        // Fall back to system default if, for some reason, resource load fails.
        HINSTANCE hinst = GetModuleHandleW(nullptr);
        HICON app_icon = LoadIconW(hinst, MAKEINTRESOURCEW(1));
        nid_.hIcon = app_icon ? app_icon : LoadIcon(nullptr, IDI_APPLICATION);
        wcscpy_s(nid_.szTip, L"Clew");
        Shell_NotifyIconW(NIM_ADD, &nid_);
        tray_created_ = true;
    }

    void remove_tray_icon() {
        if (!tray_created_) return;
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        tray_created_ = false;
    }

    void show_tray_menu() {
        POINT pt;
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"Show");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void restore_window() {
        ShowWindow(hwnd_, SW_SHOW);
        ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
    }

    // ---- Hit test for resize edges (title bar drag handled by WebView2 NonClientRegion) ----
    LRESULT handle_nchittest(HWND hwnd, LPARAM lparam) {
        POINT pt = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Don't handle resize edges when maximized
        if (IsZoomed(hwnd)) return HTCLIENT;

        int border = 6;
        UINT dpi = GetDpiForWindow(hwnd);
        if (dpi > 0) border = MulDiv(6, dpi, 96);
        bool left   = pt.x < border;
        bool right  = pt.x >= rc.right - border;
        bool top    = pt.y < border;
        bool bottom = pt.y >= rc.bottom - border;

        if (top && left)     return HTTOPLEFT;
        if (top && right)    return HTTOPRIGHT;
        if (bottom && left)  return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left)            return HTLEFT;
        if (right)           return HTRIGHT;
        if (top)             return HTTOP;
        if (bottom)          return HTBOTTOM;

        return HTCLIENT;  // WebView2 handles the rest (drag regions via CSS)
    }

    // WM_NCCALCSIZE handler — remove the system title bar and clamp the
    // proposed rect to the monitor's work area when the window is maximized
    // (otherwise it would extend over the taskbar).
    static LRESULT handle_nccalcsize(NCCALCSIZE_PARAMS* params) {
        auto& rect = params->rgrc[0];
        HMONITOR mon = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi) &&
            rect.left <= mi.rcMonitor.left && rect.top <= mi.rcMonitor.top &&
            rect.right >= mi.rcMonitor.right && rect.bottom >= mi.rcMonitor.bottom) {
            rect = mi.rcWork;
        }
        return 0;
    }

    LRESULT on_size(WPARAM wparam) {
        resize_webview();
        if (initialized_) {
            execute_script(wparam == SIZE_MAXIMIZED
                ? L"document.documentElement.classList.add('maximized')"
                : L"document.documentElement.classList.remove('maximized')");
        }
        return 0;
    }

    LRESULT on_close(HWND hwnd) {
        if (close_to_tray_) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        if (on_close_) on_close_();
        DestroyWindow(hwnd);
        return 0;
    }

    LRESULT on_destroy() {
        remove_tray_icon();
        PostQuitMessage(0);
        return 0;
    }

    static LRESULT on_getminmaxinfo(LPARAM lparam) {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
        mmi->ptMinTrackSize.x = 900;
        mmi->ptMinTrackSize.y = 600;
        return 0;
    }

    LRESULT on_exitsizemove(HWND hwnd) {
        if (on_move_resize_) {
            RECT r;
            GetWindowRect(hwnd, &r);
            on_move_resize_(r.left, r.top, r.right - r.left, r.bottom - r.top);
        }
        return 0;
    }

    LRESULT on_trayicon(LPARAM lparam) {
        if (LOWORD(lparam) == WM_LBUTTONUP) {
            restore_window();
        } else if (LOWORD(lparam) == WM_RBUTTONUP) {
            show_tray_menu();
        }
        return 0;
    }

    LRESULT on_command(HWND hwnd, WPARAM wparam) {
        if (LOWORD(wparam) == IDM_TRAY_SHOW) {
            restore_window();
        } else if (LOWORD(wparam) == IDM_TRAY_EXIT) {
            if (on_close_) on_close_();
            DestroyWindow(hwnd);
        }
        return 0;
    }

    // Runs on the UI thread. Takes ownership of the heap-allocated payload
    // smuggled through the LPARAM, builds the wire-format JSON envelope
    // ({event, data}), and hands it to WebView2 for delivery to the frontend.
    LRESULT on_push_to_frontend(LPARAM lparam) {
        std::unique_ptr<push_payload> p(reinterpret_cast<push_payload*>(lparam));
        if (!p) return 0;

#ifdef CLEW_HAS_WEBVIEW2
        if (!webview_) return 0;

        // Compose envelope. p->json_body is already a JSON value (object / array).
        std::string envelope;
        envelope.reserve(p->event.size() + p->json_body.size() + 32);
        envelope.append(R"({"event":")")
                .append(p->event)
                .append(R"(","data":)")
                .append(p->json_body)
                .append(R"(})");

        // UTF-8 → UTF-16 for PostWebMessageAsJson.
        int wlen = MultiByteToWideChar(CP_UTF8, 0, envelope.c_str(),
                                       static_cast<int>(envelope.size()),
                                       nullptr, 0);
        if (wlen <= 0) return 0;
        std::wstring wenvelope(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, envelope.c_str(),
                            static_cast<int>(envelope.size()),
                            wenvelope.data(), wlen);

        webview_->PostWebMessageAsJson(wenvelope.c_str());
#endif
        return 0;
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_CREATE) {
            auto* cs  = reinterpret_cast<CREATESTRUCT*>(lparam);
            auto* app = static_cast<webview_app*>(cs->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            return DefWindowProc(hwnd, msg, wparam, lparam);
        }

        // WM_NCCALCSIZE doesn't need an app pointer; handle before the lookup.
        if (msg == WM_NCCALCSIZE && wparam == TRUE) {
            return handle_nccalcsize(reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam));
        }
        // Suppress background erase to prevent white flash.
        if (msg == WM_ERASEBKGND) return 1;

        auto* app = reinterpret_cast<webview_app*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (!app) return DefWindowProc(hwnd, msg, wparam, lparam);

        switch (msg) {
            case WM_NCHITTEST: {
                LRESULT hit = app->handle_nchittest(hwnd, lparam);
                // Fall through to DefWindowProc so WebView2 gets drag regions.
                if (hit != HTCLIENT) return hit;
                break;
            }
            case WM_SIZE:               return app->on_size(wparam);
            case WM_CLOSE:              return app->on_close(hwnd);
            case WM_DESTROY:            return app->on_destroy();
            case WM_GETMINMAXINFO:      return on_getminmaxinfo(lparam);
            case WM_EXITSIZEMOVE:       return app->on_exitsizemove(hwnd);
            case WM_TRAYICON:           return app->on_trayicon(lparam);
            case WM_COMMAND:            return app->on_command(hwnd, wparam);
            case WM_PUSH_TO_FRONTEND:   return app->on_push_to_frontend(lparam);
            default:                    break;
        }
        return DefWindowProc(hwnd, msg, wparam, lparam);
    }

public:
    explicit webview_app(const std::wstring& url, int width = 1200, int height = 800)
        : url_(url), width_(width), height_(height) {}

    ~webview_app() {
        remove_tray_icon();
#ifdef CLEW_HAS_WEBVIEW2
        webview_controller_.Reset();
        webview_.Reset();
#endif
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
    }

    void set_title(std::wstring_view title) { title_ = title; }
    void set_on_close(std::function<void()> callback) { on_close_ = std::move(callback); }
    void set_on_move_resize(std::function<void(int, int, int, int)> callback) { on_move_resize_ = std::move(callback); }
    void set_initial_rect(int x, int y, int w, int h) { init_x_ = x; init_y_ = y; width_ = w; height_ = h; }
    void set_close_to_tray(bool v) { close_to_tray_ = v; }
    void set_devtools_enabled(bool v) { devtools_enabled_ = v; }
    // Fires on the UI thread when the frontend posts {"type":"ready"} after
    // mount. The callback typically posts a strand task that calls
    // process_projection::replay_to_frontend to deliver the current snapshot.
    void set_on_ready(std::function<void()> callback) { on_ready_ = std::move(callback); }

    // frontend_push_sink — callable from any thread. Marshals onto the UI
    // thread via PostMessageW, where on_push_to_frontend takes ownership
    // of the payload and forwards via PostWebMessageAsJson.
    void push(std::string_view event, std::string json_body) override {
        if (!hwnd_) return;
        auto p = std::make_unique<push_payload>();
        p->event.assign(event.data(), event.size());
        p->json_body = std::move(json_body);

        LPARAM lp = reinterpret_cast<LPARAM>(p.release());
        if (!::PostMessageW(hwnd_, WM_PUSH_TO_FRONTEND, 0, lp)) {
            // HWND invalid (shutdown / not yet created): take ownership back.
            delete reinterpret_cast<push_payload*>(lp);
        }
    }

    bool create(HINSTANCE hinstance) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hinstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = L"ClewWebViewClass";
        // Embedded icon (resource ID 1 from assets/clew.rc) — drives the title
        // bar icon (hIconSm) and the Alt-Tab / taskbar big icon (hIcon).
        HICON embedded = LoadIconW(hinstance, MAKEINTRESOURCEW(1));
        wc.hIcon = embedded ? embedded : LoadIcon(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;

        if (!RegisterClassExW(&wc)) {
            DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                PC_LOG_ERROR("Failed to register window class: {}", error);
                return false;
            }
        }

        int x;
        int y;
        if (init_x_ >= 0 && init_y_ >= 0) {
            x = init_x_;
            y = init_y_;
        } else {
            int screen_width = GetSystemMetrics(SM_CXSCREEN);
            int screen_height = GetSystemMetrics(SM_CYSCREEN);
            x = (screen_width - width_) / 2;
            y = (screen_height - height_) / 2;
        }

        hwnd_ = CreateWindowExW(
            0, L"ClewWebViewClass", title_.c_str(),
            WS_OVERLAPPEDWINDOW,
            x, y, width_, height_,
            nullptr, nullptr, hinstance, this
        );

        if (!hwnd_) {
            PC_LOG_ERROR("Failed to create window: {}", GetLastError());
            return false;
        }

        // Trigger WM_NCCALCSIZE to apply our frameless layout
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
            SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        // DWM: extend frame into client area for window shadow
        MARGINS margins = {1, 1, 1, 1};
        DwmExtendFrameIntoClientArea(hwnd_, &margins);

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        create_tray_icon();
        initialize_webview();

        PC_LOG_INFO("WebView window created ({}x{}, frameless)", width_, height_);
        return true;
    }

    int run() {
        MSG msg = {};
        while (GetMessage(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

    HWND get_hwnd() const { return hwnd_; }
    bool is_initialized() const { return initialized_; }

    void navigate(const std::wstring& url) {
        url_ = url;
#ifdef CLEW_HAS_WEBVIEW2
        if (webview_) webview_->Navigate(url.c_str());
#endif
    }

    void execute_script(const std::wstring& script) {
#ifdef CLEW_HAS_WEBVIEW2
        if (webview_) webview_->ExecuteScript(script.c_str(), nullptr);
#else
        (void)script;
#endif
    }
};

inline bool is_webview2_available() {
#ifdef CLEW_HAS_WEBVIEW2
    LPWSTR version = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    if (SUCCEEDED(hr) && version) {
        // Proper UTF-16 -> UTF-8 conversion (avoids C4244 narrowing from the
        // iterator-based std::string constructor).
        int len = WideCharToMultiByte(CP_UTF8, 0, version, -1, nullptr, 0, nullptr, nullptr);
        std::string v;
        if (len > 1) {
            v.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, version, -1, v.data(), len, nullptr, nullptr);
        }
        PC_LOG_INFO("WebView2 runtime version: {}", v);
        CoTaskMemFree(version);
        return true;
    }
    PC_LOG_WARN("WebView2 runtime not available.");
    return false;
#else
    PC_LOG_WARN("WebView2 support not compiled in");
    return false;
#endif
}

} // namespace clew
