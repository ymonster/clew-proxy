#include "config/config_store.hpp"

#include <utility>

namespace clew {

config_store::config_store(config_manager& mgr) : mgr_(mgr) {}

ConfigV2 config_store::get() const {
    std::scoped_lock lock(mutex_);
    return mgr_.get_v2();
}

std::string config_store::raw_json() const {
    std::scoped_lock lock(mutex_);
    return mgr_.get_raw_config();
}

void config_store::replace_from_json(std::string_view raw) {
    ConfigV2 after;
    {
        std::scoped_lock lock(mutex_);
        // config_manager::set_raw_config returns "" on success, else
        // a human-readable error string. It persists internally via save().
        if (std::string err = mgr_.set_raw_config(std::string{raw}); !err.empty()) {
            throw api_exception{api_error::invalid_argument, std::move(err)};
        }
        after = mgr_.get_v2();
    }
    for (const auto& ob : observers_) {
        ob(after, config_change::wholesale_replaced);
    }
}

void config_store::subscribe(observer ob) {
    std::scoped_lock lock(mutex_);
    observers_.push_back(std::move(ob));
}

} // namespace clew
