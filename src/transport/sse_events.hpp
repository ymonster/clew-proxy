#pragma once

// Canonical SSE event names. DESIGN H5 locks this taxonomy to exactly
// three events; adding a new event goes through a separate refactor.

#include <string_view>

namespace clew::sse_events {

inline constexpr std::string_view process_update    = "process_update";
inline constexpr std::string_view process_exit      = "process_exit";
inline constexpr std::string_view auto_rule_changed = "auto_rule_changed";

} // namespace clew::sse_events
