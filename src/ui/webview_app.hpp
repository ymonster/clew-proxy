#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>
#include <string>
#include <functional>
#include "core/log.hpp"

#pragma comment(lib, "dwmapi.lib")

#ifdef CLEW_HAS_WEBVIEW2
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#endif

namespace clew {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT IDM_TRAY_SHOW = 40001;
constexpr UINT IDM_TRAY_EXIT = 40002;

class webview_app {
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
        options->put_AdditionalBrowserArguments(L"--no-proxy-server");

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

                                    // Enable non-client region support for CSS app-region: drag
                                    Microsoft::WRL::ComPtr<ICoreWebView2Settings9> settings9;
                                    if (SUCCEEDED(settings->QueryInterface(IID_PPV_ARGS(&settings9)))) {
                                        settings9->put_IsNonClientRegionSupportEnabled(TRUE);
                                        PC_LOG_INFO("WebView2 NonClientRegion support enabled");
                                    }

                                    settings->Release();
                                }

                                // Web message handler for window control commands
                                webview_->add_WebMessageReceived(
                                    Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                        [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                            LPWSTR msg = nullptr;
                                            args->TryGetWebMessageAsString(&msg);
                                            if (!msg) return S_OK;
                                            std::wstring cmd(msg);
                                            CoTaskMemFree(msg);

                                            if (cmd == L"minimize") {
                                                ShowWindow(hwnd_, SW_MINIMIZE);
                                            } else if (cmd == L"maximize") {
                                                ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
                                            } else if (cmd == L"close") {
                                                SendMessage(hwnd_, WM_CLOSE, 0, 0);
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

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        webview_app* app = nullptr;
        if (msg == WM_CREATE) {
            CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lparam);
            app = static_cast<webview_app*>(cs->lpCreateParams);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        } else {
            app = reinterpret_cast<webview_app*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        }

        // WM_NCCALCSIZE: remove system title bar; handle maximized state properly
        if (msg == WM_NCCALCSIZE && wparam == TRUE) {
            auto& rect = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0];
            HMONITOR mon = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            // Detect maximize: proposed rect covers or exceeds the full monitor rect
            if (GetMonitorInfoW(mon, &mi) &&
                rect.left <= mi.rcMonitor.left && rect.top <= mi.rcMonitor.top &&
                rect.right >= mi.rcMonitor.right && rect.bottom >= mi.rcMonitor.bottom) {
                rect = mi.rcWork;  // constrain to work area (respects taskbar)
            }
            return 0;
        }

        // Suppress background erase to prevent white flash
        if (msg == WM_ERASEBKGND) return 1;

        if (app) {
            switch (msg) {

                case WM_NCHITTEST: {
                    LRESULT hit = app->handle_nchittest(hwnd, lparam);
                    if (hit != HTCLIENT) return hit;
                    // Fall through to DefWindowProc for WebView2 to handle drag regions
                    break;
                }

                case WM_SIZE:
                    app->resize_webview();
                    // Notify frontend of maximize state for padding adjustment
                    if (app->initialized_) {
                        if (wparam == SIZE_MAXIMIZED)
                            app->execute_script(L"document.documentElement.classList.add('maximized')");
                        else
                            app->execute_script(L"document.documentElement.classList.remove('maximized')");
                    }
                    return 0;

                case WM_CLOSE:
                    if (app->close_to_tray_) {
                        ShowWindow(hwnd, SW_HIDE);
                        return 0;
                    }
                    if (app->on_close_) app->on_close_();
                    DestroyWindow(hwnd);
                    return 0;

                case WM_DESTROY:
                    app->remove_tray_icon();
                    PostQuitMessage(0);
                    return 0;

                case WM_GETMINMAXINFO: {
                    MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
                    mmi->ptMinTrackSize.x = 900;
                    mmi->ptMinTrackSize.y = 600;
                    return 0;
                }

                case WM_EXITSIZEMOVE:
                    if (app->on_move_resize_) {
                        RECT r;
                        GetWindowRect(hwnd, &r);
                        app->on_move_resize_(r.left, r.top, r.right - r.left, r.bottom - r.top);
                    }
                    return 0;

                case WM_TRAYICON:
                    if (LOWORD(lparam) == WM_LBUTTONUP) {
                        app->restore_window();
                    } else if (LOWORD(lparam) == WM_RBUTTONUP) {
                        app->show_tray_menu();
                    }
                    return 0;

                case WM_COMMAND:
                    if (LOWORD(wparam) == IDM_TRAY_SHOW) {
                        app->restore_window();
                    } else if (LOWORD(wparam) == IDM_TRAY_EXIT) {
                        if (app->on_close_) app->on_close_();
                        DestroyWindow(hwnd);
                    }
                    return 0;

                default:
                    break;  // fall through to DefWindowProc
            }
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
