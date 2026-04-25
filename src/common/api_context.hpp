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
class shell_service;
class stats_service;
class sse_hub;

struct api_context {
    config_service&       config;
    connection_service&   connections;
    group_service&        groups;
    icon_service&         icons;
    process_tree_service& processes;
    rule_service&         rules;
    shell_service&        shell;
    stats_service&        stats;
    sse_hub&              sse;
};

} // namespace clew
