#pragma once

// config_push_bridge — subscribes to config_store and turns each
// config_change tag into a frontend `auto_rule_changed` push with an
// `action` field. Construction-time wiring only; the sink can be set
// later (after the WebView2 host is created in app.cpp).

#include <atomic>

namespace clew {

class config_store;
class frontend_push_sink;

class config_sse_bridge {
public:
    explicit config_sse_bridge(config_store& cfg);

    config_sse_bridge(const config_sse_bridge&)            = delete;
    config_sse_bridge& operator=(const config_sse_bridge&) = delete;

    void set_sink(frontend_push_sink* sink) noexcept { sink_.store(sink); }

private:
    std::atomic<frontend_push_sink*> sink_{nullptr};
};

} // namespace clew
