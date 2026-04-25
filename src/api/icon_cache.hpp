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
#include <string_view>
#include <vector>
#include <mutex>
#include <ranges>
#include <cctype>
#include "core/log.hpp"
#include "core/scoped_exit.hpp"
#include "core/string_hash.hpp"

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
    const std::vector<uint8_t>& get_icon(std::string_view exe_path) {
        {
            // Heterogeneous find: lowercase view built on stack, no std::string alloc on hit
            std::string probe = normalize_key(exe_path);
            std::scoped_lock lock(mutex_);
            auto it = cache_.find(probe);
            if (it != cache_.end()) return it->second;
        }

        // Cache miss — extract icon (may be slow, ~5ms)
        auto png = extract_icon_png(exe_path);
        std::string key = normalize_key(exe_path);

        std::scoped_lock lock(mutex_);
        auto [it, _] = cache_.try_emplace(std::move(key), std::move(png));
        return it->second;
    }

    // Lookup by exe name: find a cached entry whose key ends with the given name.
    // Falls back to extracting from a resolved path if provided.
    const std::vector<uint8_t>& get_icon_by_name(std::string_view exe_name,
                                                 std::string_view fallback_path = {}) {
        std::string name_lower = normalize_key(exe_name);

        {
            std::scoped_lock lock(mutex_);
            // Heterogeneous find on name_cache_ (no std::string construction on hit path)
            auto it = name_cache_.find(std::string_view{name_lower});
            if (it != name_cache_.end()) {
                auto cit = cache_.find(std::string_view{it->second});
                if (cit != cache_.end()) return cit->second;
            }
        }

        // Need to find the exe path for this name
        std::string resolved_path = fallback_path.empty()
            ? find_exe_path(exe_name)
            : std::string{fallback_path};

        if (resolved_path.empty()) {
            // Return empty (cached to avoid repeated lookups)
            std::scoped_lock lock(mutex_);
            name_cache_[std::move(name_lower)] = "";
            static const std::vector<uint8_t> empty;
            return empty;
        }

        // Extract and cache
        const auto& result = get_icon(resolved_path);

        std::scoped_lock lock(mutex_);
        name_cache_[std::move(name_lower)] = normalize_key(resolved_path);
        return result;
    }

private:
    ULONG_PTR gdiplus_token_ = 0;
    CLSID png_clsid_{};
    bool has_png_encoder_ = false;

    std::mutex mutex_;
    clew::string_map<std::vector<uint8_t>> cache_;       // key: lowercase path
    clew::string_map<std::string>          name_cache_;  // exe name → path key

    [[nodiscard]] static std::string normalize_key(std::string_view path) {
        std::string key{path};
        std::ranges::transform(key, key.begin(),
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

    static std::string find_exe_path(std::string_view exe_name) {
        // Convert name to wide (use explicit length; string_view is not null-terminated)
        const auto src_len = static_cast<int>(exe_name.size());
        int wlen = MultiByteToWideChar(CP_UTF8, 0, exe_name.data(), src_len, nullptr, 0);
        if (wlen <= 0) return {};
        std::wstring wname(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, exe_name.data(), src_len, wname.data(), wlen);

        // Snapshot processes
        auto snap = wrap_handle(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snap) return {};

        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (!Process32FirstW(snap.get(), &pe)) return {};

        do {
            if (_wcsicmp(pe.szExeFile, wname.c_str()) != 0) continue;
            auto h = wrap_handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                             FALSE, pe.th32ProcessID));
            if (!h) continue;
            auto result = exe_path_from_handle(h.get());
            if (!result.empty()) return result;
        } while (Process32NextW(snap.get(), &pe));

        return {};
    }

    // Extract icon from exe path, convert to PNG bytes
    std::vector<uint8_t> extract_icon_png(std::string_view exe_path) {
        // Convert to wide (explicit length; string_view is not null-terminated)
        const auto src_len = static_cast<int>(exe_path.size());
        int wlen = MultiByteToWideChar(CP_UTF8, 0, exe_path.data(), src_len, nullptr, 0);
        if (wlen <= 0) return {};
        std::wstring wpath(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, exe_path.data(), src_len, wpath.data(), wlen);

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

        std::unique_ptr<Gdiplus::Bitmap> bmp(Gdiplus::Bitmap::FromHICON(hIcon));
        if (!bmp) return result;

        constexpr int SIZE = 16;
        Gdiplus::Bitmap resized(SIZE, SIZE, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&resized);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(bmp.get(), 0, 0, SIZE, SIZE);
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
        return result;
    }
};

} // namespace clew
