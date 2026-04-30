// /api/events — chunked Server-Sent Events stream.
//
// Each connection gets its own provider lambda running in a httplib worker
// thread. We attach the sink to sse_hub, keep the provider alive with
// periodic pings, and detach + return false when the sink becomes
// non-writable (client disconnect) or the hub is stopped (server shutdown).

#include <chrono>
#include <string_view>
#include <thread>

#include <httplib.h>

#include "common/api_context.hpp"
#include "transport/route_def.hpp"
#include "transport/route_registry.hpp"
#include "transport/sse_hub.hpp"

namespace clew {

namespace {

// Keep-alive design:
// - The provider lambda blocks until the connection ends. We can't sleep for
//   long durations: when a client disconnects we only notice on the next
//   `is_writable()` check, and the sink stays in `sse_hub` until then. With
//   a 15s sleep, a stale sink could linger 15s; combined with auto-reconnect
//   on the frontend that produced periods with two attached sinks, doubling
//   broadcast fan-out and the GET storm it triggers.
// - Polling every kPollInterval keeps stale-sink lifetime bounded by that
//   interval. Pings only need to reach the client roughly every kPingEvery
//   poll ticks (1s here is enough to keep most browser/proxy idle timers
//   happy without flooding).
constexpr auto kPollInterval = std::chrono::milliseconds(200);
constexpr int  kPingEvery    = 5;  // → ~1s between actual ping writes

void handle_events(const httplib::Request&, httplib::Response& res, const api_context& ctx) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection",    "keep-alive");

    sse_hub& hub = ctx.sse;

    res.set_chunked_content_provider(
        "text/event-stream",
        [&hub](std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
            hub.attach(&sink);

            // Initial comment flushes headers so clients see the stream open.
            static constexpr std::string_view kHello = ":connected\n\n";
            sink.write(kHello.data(), kHello.size());

            static constexpr std::string_view kPing = ":ping\n\n";
            int ticks = 0;
            while (sink.is_writable() && hub.is_running()) {
                std::this_thread::sleep_for(kPollInterval);
                if (!sink.is_writable() || !hub.is_running()) break;
                if (++ticks >= kPingEvery) {
                    sink.write(kPing.data(), kPing.size());
                    ticks = 0;
                }
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
