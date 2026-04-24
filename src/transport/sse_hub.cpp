#include "transport/sse_hub.hpp"

#include <algorithm>
#include <string>

#include <httplib.h>

namespace clew {

void sse_hub::attach(httplib::DataSink* sink) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) return;
    sinks_.push_back(sink);
}

void sse_hub::detach(httplib::DataSink* sink) {
    std::lock_guard<std::mutex> lock(mu_);
    std::erase(sinks_, sink);
}

void sse_hub::broadcast(std::string_view event, const nlohmann::json& data) {
    std::string frame;
    frame.reserve(64);
    frame.append("event: ").append(event).append("\ndata: ")
         .append(data.dump()).append("\n\n");

    std::lock_guard<std::mutex> lock(mu_);
    if (!running_) return;
    for (auto* sink : sinks_) {
        if (sink && sink->is_writable()) {
            sink->write(frame.c_str(), frame.size());
        }
    }
}

void sse_hub::stop() {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
    sinks_.clear();
}

bool sse_hub::is_running() const {
    std::lock_guard<std::mutex> lock(mu_);
    return running_;
}

} // namespace clew
