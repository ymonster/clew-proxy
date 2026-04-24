#include "projection/config_sse_bridge.hpp"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "config/config_change_tag.hpp"
#include "config/config_store.hpp"
#include "config/types.hpp"
#include "transport/sse_events.hpp"
#include "transport/sse_hub.hpp"

namespace clew {

namespace {

std::string_view tag_to_action(config_change tag) noexcept {
    switch (tag) {
        case config_change::rule_created:       return "created";
        case config_change::rule_updated:       return "updated";
        case config_change::rule_deleted:       return "deleted";
        case config_change::rule_excluded:      return "exclude";
        case config_change::rule_unexcluded:    return "unexclude";
        case config_change::group_created:      return "group_created";
        case config_change::group_updated:      return "group_updated";
        case config_change::group_deleted:      return "group_deleted";
        case config_change::group_migrated:     return "migrated";
        case config_change::wholesale_replaced: return "config_reload";
    }
    return "unknown";
}

} // namespace

config_sse_bridge::config_sse_bridge(config_store& cfg, sse_hub& sse) : sse_(sse) {
    cfg.subscribe([this](const ConfigV2&, config_change tag) {
        sse_.broadcast(
            sse_events::auto_rule_changed,
            nlohmann::json{{"action", std::string(tag_to_action(tag))}});
    });
}

} // namespace clew
