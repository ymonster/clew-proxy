#pragma once

// config_change tag — passed through config_store.mutate() so observers can
// distinguish what changed without re-diffing the ConfigV2 struct.
//
// Kept in its own header to minimize include coupling (observers that only
// need this tag don't need the full config_store API).

namespace clew {

enum class config_change {
    rule_created,
    rule_updated,
    rule_deleted,
    rule_excluded,
    rule_unexcluded,
    group_created,
    group_updated,
    group_deleted,
    group_migrated,
    wholesale_replaced,   // PUT /api/config full replacement
};

} // namespace clew
