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
    using enum config_change;
    switch (tag) {
        case rule_created:       return "created";
        case rule_updated:       return "updated";
        case rule_deleted:       return "deleted";
        case rule_excluded:      return "exclude";
        case rule_unexcluded:    return "unexclude";
        case group_created:      return "group_created";
        case group_updated:      return "group_updated";
        case group_deleted:      return "group_deleted";
        case group_migrated:     return "migrated";
        case wholesale_replaced: return "config_reload";
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
