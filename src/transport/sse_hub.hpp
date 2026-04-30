#pragma once

// SSE (Server-Sent Events) fan-out hub.
//
// Clients register via attach() from the chunked content provider of
// GET /api/events; the hub writes to every still-live sink. Writes to
// non-writable sinks silently fail — httplib reaps the corresponding
// provider when its return value becomes false.
//
// DESIGN H5 hard constraint: service code is FORBIDDEN from invoking
// broadcast() directly. All SSE events are derived from state holders:
//   - process_projection    -> process_update
//   - config_sse_bridge     -> auto_rule_changed

#include <mutex>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace httplib { struct DataSink; }

namespace clew {

class sse_hub {
public:
    sse_hub() = default;
    ~sse_hub() = default;

    sse_hub(const sse_hub&)            = delete;
    sse_hub& operator=(const sse_hub&) = delete;

    void attach(httplib::DataSink* sink);
    void detach(httplib::DataSink* sink);

    // Writes an SSE frame to every currently-attached sink.
    void broadcast(std::string_view event, const nlohmann::json& data);

    // Stops accepting new sinks and clears the current roster so any
    // in-flight chunked providers see no sinks and return.
    void stop();

    [[nodiscard]] bool is_running() const;

private:
    mutable std::mutex               mu_;
    std::vector<httplib::DataSink*>  sinks_;
    bool                             running_{true};
};

} // namespace clew
