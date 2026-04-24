#include "services/shell_service.hpp"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "common/api_exception.hpp"

namespace clew {

nlohmann::json shell_service::browse_exe() {
    char file_path[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = nullptr;
    ofn.lpstrFilter = "Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = file_path;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameA(&ofn)) {
        nlohmann::json j;
        j["cancelled"] = true;
        return j;
    }

    std::string full(file_path);
    std::string dir, name;
    auto last_sep = full.find_last_of("\\/");
    if (last_sep != std::string::npos) {
        dir  = full.substr(0, last_sep + 1);
        name = full.substr(last_sep + 1);
    } else {
        name = full;
    }

    nlohmann::json j;
    j["path"] = full;
    j["dir"]  = dir;
    j["name"] = name;
    return j;
}

void shell_service::reveal(std::string_view path) {
    if (path.empty()) {
        throw api_exception{api_error::invalid_argument, "path is required"};
    }

    std::string utf8{path};
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (needed <= 0) {
        throw api_exception{api_error::invalid_argument, "invalid UTF-8 path"};
    }
    std::wstring wpath(static_cast<std::size_t>(needed) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wpath.data(), needed);

    std::wstring wcmd = L"/select,\"" + wpath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", wcmd.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace clew
