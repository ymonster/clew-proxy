#pragma once

// api_context — dependency-injection aggregate passed to the transport
// layer. Each HTTP handler dispatches into ctx.<service>.method(...).
// Populated in main.cpp after all service + bridge objects are constructed.

namespace clew {

class config_service;
class connection_service;
class group_service;
class icon_service;
class process_tree_service;
class rule_service;
class stats_service;

// shell_service is intentionally NOT here — it has only static methods,
// so handlers call clew::shell_service::method() directly.
//
// SSE was retired when the backend->frontend push channel switched to
// WebView2 PostMessage; handlers no longer reach a broadcast hub. State
// holders (process_projection / config_sse_bridge) push directly via the
// frontend_push_sink they hold.
struct api_context {
    config_service&       config;
    connection_service&   connections;
    group_service&        groups;
    icon_service&         icons;
    process_tree_service& processes;
    rule_service&         rules;
    stats_service&        stats;
};

} // namespace clew
