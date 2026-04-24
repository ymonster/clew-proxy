#pragma once

// config_sse_bridge — subscribes to config_store and turns each config_change
// tag into an SSE `auto_rule_changed` frame with an `action` field.
// Construction-only wiring; no other API surface.

namespace clew {

class config_store;
class sse_hub;

class config_sse_bridge {
public:
    config_sse_bridge(config_store& cfg, sse_hub& sse);

    config_sse_bridge(const config_sse_bridge&)            = delete;
    config_sse_bridge& operator=(const config_sse_bridge&) = delete;

private:
    sse_hub& sse_;
};

} // namespace clew
