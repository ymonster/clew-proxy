#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <appmodel.h>
#include <tlhelp32.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include "core/log.hpp"

#pragma comment(lib, "gdiplus.lib")

namespace clew {

class icon_cache {
public:
    bool init() {
        Gdiplus::GdiplusStartupInput input;
        auto status = Gdiplus::GdiplusStartup(&gdiplus_token_, &input, nullptr);
        if (status != Gdiplus::Ok) {
            PC_LOG_ERROR("GDI+ initialization failed: {}", (int)status);
            return false;
        }

        // Cache PNG encoder CLSID
        UINT num = 0;
        UINT size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        if (size > 0) {
            auto buf = std::make_unique<uint8_t[]>(size);
            auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.get());
            Gdiplus::GetImageEncoders(num, size, encoders);
            for (UINT i = 0; i < num; i++) {
                if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
                    png_clsid_ = encoders[i].Clsid;
                    has_png_encoder_ = true;
                    break;
                }
            }
        }

        PC_LOG_INFO("Icon cache initialized (GDI+ OK, PNG encoder {})",
                      has_png_encoder_ ? "found" : "NOT found");
        return has_png_encoder_;
    }

    void shutdown() {
        if (gdiplus_token_) {
            Gdiplus::GdiplusShutdown(gdiplus_token_);
            gdiplus_token_ = 0;
        }
    }

    // Get PNG icon bytes for an exe path. Thread-safe, cached.
    // Returns empty vector on failure.
    const std::vector<uint8_t>& get_icon(const std::string& exe_path) {
        std::string key = normalize_key(exe_path);

        {
            std::lock_guard lock(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end()) return it->second;
        }

        // Cache miss — extract icon (may be slow, ~5ms)
        auto png = extract_icon_png(exe_path);

        std::lock_guard lock(mutex_);
        auto [it, _] = cache_.emplace(key, std::move(png));
        return it->second;
    }

    // Lookup by exe name: find a cached entry whose key ends with the given name.
    // Falls back to extracting from a resolved path if provided.
    const std::vector<uint8_t>& get_icon_by_name(const std::string& exe_name,
                                                   const std::string& fallback_path = {}) {
        std::string name_lower = exe_name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
            [](unsigned char c) { return std::tolower(c); });

        {
            std::lock_guard lock(mutex_);
            // Check if we already have this name cached
            auto it = name_cache_.find(name_lower);
            if (it != name_cache_.end()) {
                auto cit = cache_.find(it->second);
                if (cit != cache_.end()) return cit->second;
            }
        }

        // Need to find the exe path for this name
        std::string resolved_path = fallback_path;
        if (resolved_path.empty()) {
            resolved_path = find_exe_path(exe_name);
        }

        if (resolved_path.empty()) {
            // Return empty (cached to avoid repeated lookups)
            std::lock_guard lock(mutex_);
            name_cache_[name_lower] = "";
            static const std::vector<uint8_t> empty;
            return empty;
        }

        // Extract and cache
        const auto& result = get_icon(resolved_path);

        std::lock_guard lock(mutex_);
        name_cache_[name_lower] = normalize_key(resolved_path);
        return result;
    }

private:
    ULONG_PTR gdiplus_token_ = 0;
    CLSID png_clsid_{};
    bool has_png_encoder_ = false;

    std::mutex mutex_;
    std::unordered_map<std::string, std::vector<uint8_t>> cache_;        // key: lowercase path
    std::unordered_map<std::string, std::string> name_cache_;            // exe name -> path key

    static std::string normalize_key(const std::string& path) {
        std::string key = path;
        std::transform(key.begin(), key.end(), key.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return key;
    }

    // Find exe path by name using CreateToolhelp32Snapshot
    // Get UTF-8 full image path from an opened process handle. Empty on failure.
    static std::string exe_path_from_handle(HANDLE h) {
        WCHAR path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (!QueryFullProcessImageNameW(h, 0, path, &len)) return {};
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, path, len, nullptr, 0, nullptr, nullptr);
        if (utf8_len <= 0) return {};
        std::string result(utf8_len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, path, len, result.data(), utf8_len, nullptr, nullptr);
        return result;
    }

    static std::string find_exe_path(const std::string& exe_name) {
        // Convert name to wide
        int wlen = MultiByteToWideChar(CP_UTF8, 0, exe_name.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return {};
        std::wstring wname(wlen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, exe_name.c_str(), -1, wname.data(), wlen);

        // Snapshot processes
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return {};

        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        std::string result;

        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, wname.c_str()) == 0) {
                    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                    if (h && h != INVALID_HANDLE_VALUE) {
                        result = exe_path_from_handle(h);
                        CloseHandle(h);
                    }
                    if (!result.empty()) break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return result;
    }

    // Extract icon from exe path, convert to PNG bytes
    std::vector<uint8_t> extract_icon_png(const std::string& exe_path) {
        // Convert to wide
        int wlen = MultiByteToWideChar(CP_UTF8, 0, exe_path.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return {};
        std::wstring wpath(wlen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, exe_path.c_str(), -1, wpath.data(), wlen);

        // Try SHGetFileInfoW first
        HICON hIcon = nullptr;
        SHFILEINFOW sfi = {};
        if (SHGetFileInfoW(wpath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) {
            hIcon = sfi.hIcon;
        }

        // Fallback: ExtractIconExW
        if (!hIcon) {
            HICON hSmall = nullptr;
            if (ExtractIconExW(wpath.c_str(), 0, nullptr, &hSmall, 1) > 0) {
                hIcon = hSmall;
            }
        }

        if (!hIcon) return {};

        auto png = hicon_to_png(hIcon);
        DestroyIcon(hIcon);
        return png;
    }

    // Convert HICON to PNG bytes via GDI+
    std::vector<uint8_t> hicon_to_png(HICON hIcon) {
        std::vector<uint8_t> result;
        if (!has_png_encoder_) return result;

        auto* bmp = Gdiplus::Bitmap::FromHICON(hIcon);
        if (!bmp) return result;

        constexpr int SIZE = 16;
        Gdiplus::Bitmap resized(SIZE, SIZE, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&resized);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(bmp, 0, 0, SIZE, SIZE);
        }

        IStream* stream = nullptr;
        CreateStreamOnHGlobal(nullptr, TRUE, &stream);
        if (stream) {
            if (resized.Save(stream, &png_clsid_, nullptr) == Gdiplus::Ok) {
                LARGE_INTEGER zero = {};
                stream->Seek(zero, STREAM_SEEK_SET, nullptr);
                STATSTG stat;
                stream->Stat(&stat, STATFLAG_NONAME);
                ULONG png_size = (ULONG)stat.cbSize.QuadPart;
                result.resize(png_size);
                ULONG read = 0;
                stream->Read(result.data(), png_size, &read);
                result.resize(read);
            }
            stream->Release();
        }

        delete bmp;
        return result;
    }
};

} // namespace clew
