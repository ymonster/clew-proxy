#include "services/config_service.hpp"

#include "config/config_store.hpp"

namespace clew {

config_service::config_service(config_store& cfg) : cfg_(cfg) {}

std::string config_service::get_raw() const {
    return cfg_.raw_json();
}

void config_service::replace_raw(std::string_view raw) {
    cfg_.replace_from_json(raw);
}

} // namespace clew
