#pragma once

// shell_service — native Win32 shell helpers used by the frontend
// (native exe picker + "Reveal in Explorer"). Stateless.

#include <string_view>

#include <nlohmann/json.hpp>

namespace clew {

class shell_service {
public:
    // Opens the native OPENFILENAME modal. Blocks on GUI thread.
    // Returns {path, dir, name} on success, {cancelled:true} if the user
    // cancels the dialog.
    [[nodiscard]] static nlohmann::json browse_exe();

    // Launches Explorer with /select on the given UTF-8 path.
    // Throws api_exception{invalid_argument} if path is empty.
    static void reveal(std::string_view path);
};

} // namespace clew
