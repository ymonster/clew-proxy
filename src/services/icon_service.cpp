#include "services/icon_service.hpp"

#include "api/icon_cache.hpp"
#include "common/api_exception.hpp"

namespace clew {

icon_service::icon_service(icon_cache& cache) : cache_(cache) {}

std::vector<std::uint8_t> icon_service::get(std::string_view name, std::string_view path) {
    const auto& png = cache_.get_icon_by_name(name, path);
    if (png.empty()) {
        throw api_exception{api_error::not_found, "icon not found"};
    }
    return png;
}

} // namespace clew
