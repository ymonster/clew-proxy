// /api/events — chunked Server-Sent Events stream.
//
// Each connection gets its own provider lambda running in a httplib worker
// thread. We attach the sink to sse_hub, keep the provider alive with
// periodic pings, and detach + return false when the sink becomes
// non-writable (client disconnect) or the hub is stopped (server shutdown).

#include <chrono>
#include <cstring>
#include <thread>

#include <httplib.h>

#include "common/api_context.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"
#include "transport/sse_hub.hpp"

namespace clew {

namespace {

constexpr auto kKeepAliveInterval = std::chrono::seconds(15);

void handle_events(const httplib::Request&, httplib::Response& res, const api_context& ctx) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection",    "keep-alive");

    sse_hub& hub = ctx.sse;

    res.set_chunked_content_provider(
        "text/event-stream",
        [&hub](std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
            hub.attach(&sink);

            // Initial comment flushes headers so clients see the stream open.
            static constexpr char kHello[] = ":connected\n\n";
            sink.write(kHello, std::strlen(kHello));

            while (sink.is_writable() && hub.is_running()) {
                std::this_thread::sleep_for(kKeepAliveInterval);
                if (!sink.is_writable() || !hub.is_running()) break;
                static constexpr char kPing[] = ":ping\n\n";
                sink.write(kPing, std::strlen(kPing));
            }

            hub.detach(&sink);
            return false;  // end of stream
        });
}

} // namespace

void register_sse_handlers(route_registry& r) {
    r.add({http_method::get, "/api/events", &handle_events});
}

} // namespace clew
