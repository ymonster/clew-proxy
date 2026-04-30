#pragma once

// Canonical SSE event names. DESIGN H5 locks this taxonomy; adding a new
// event goes through a separate refactor.
//
// Note: `process_exit` was retired in favor of `process_update` (every STOP
// already triggers the latter, and the frontend handler for both was
// identical). The taxonomy is now two events.

#include <string_view>

namespace clew::sse_events {

inline constexpr std::string_view process_update    = "process_update";
inline constexpr std::string_view auto_rule_changed = "auto_rule_changed";

} // namespace clew::sse_events
