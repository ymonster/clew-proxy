#include "transport/sse_hub.hpp"

#include <algorithm>
#include <chrono>
#include <string>

#include <httplib.h>

#include "core/log.hpp"

namespace clew {

void sse_hub::attach(httplib::DataSink* sink) {
    std::scoped_lock lock(mu_);
    if (!running_) return;
    // Dedup defensively: if the same sink pointer is already attached we
    // would double-broadcast on every event. The provider lambda in the SSE
    // handler is documented to call attach() exactly once per connection,
    // but a future change there shouldn't be able to silently produce
    // duplicate fan-out.
    if (std::find(sinks_.begin(), sinks_.end(), sink) != sinks_.end()) return;
    sinks_.push_back(sink);
}

void sse_hub::detach(httplib::DataSink* sink) {
    std::scoped_lock lock(mu_);
    std::erase(sinks_, sink);
}

void sse_hub::broadcast(std::string_view event, const nlohmann::json& data) {
    std::string frame;
    frame.reserve(64);
    frame.append("event: ").append(event).append("\ndata: ")
         .append(data.dump()).append("\n\n");

    const auto t0 = std::chrono::steady_clock::now();
    std::scoped_lock lock(mu_);
    const auto t1 = std::chrono::steady_clock::now();
    if (!running_) return;
    int writes = 0;
    int skipped = 0;
    for (const auto* sink : sinks_) {
        if (sink && sink->is_writable()) {
            sink->write(frame.c_str(), frame.size());
            ++writes;
        } else {
            ++skipped;
        }
    }
    const auto t2 = std::chrono::steady_clock::now();

    const auto lock_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const auto write_us = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
    // Always log so we can attribute every notify to its actual SSE write
    // cost. If anything is meaningfully slow, escalate to WARN so it stands
    // out without grepping. Threshold (1ms) chosen because anything above
    // that on a localhost socket is suspicious.
    if (write_us > 1000 || lock_us > 1000) {
        PC_LOG_WARN("[DIAG-SSE] event={} sinks={} writes={} skipped={} frame_bytes={} lock={}us write={}us",
                    event, sinks_.size(), writes, skipped, frame.size(), lock_us, write_us);
    } else {
        PC_LOG_INFO("[DIAG-SSE] event={} sinks={} writes={} skipped={} frame_bytes={} lock={}us write={}us",
                    event, sinks_.size(), writes, skipped, frame.size(), lock_us, write_us);
    }
}

void sse_hub::stop() {
    std::scoped_lock lock(mu_);
    running_ = false;
    sinks_.clear();
}

bool sse_hub::is_running() const {
    std::scoped_lock lock(mu_);
    return running_;
}

} // namespace clew
