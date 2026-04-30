#pragma once

// Push notification interface used by state holders (process_projection,
// config_sse_bridge) to send events to the frontend. The implementation
// (webview_app) marshals the call onto the UI thread and forwards via
// WebView2's PostWebMessageAsJson.
//
// This replaced sse_hub when the project switched the backend->frontend
// push channel from HTTP/SSE to in-process WebView2 IPC. Callers may
// invoke push() from any thread; implementations must handle the
// cross-thread hop themselves.

#include <string>
#include <string_view>

namespace clew {

class frontend_push_sink {
public:
    virtual ~frontend_push_sink() = default;

    // event:     short literal event name, e.g. "process_update".
    // json_body: already-serialized JSON payload (object or array). The sink
    //            is responsible for wrapping it as { event, data } before
    //            handing to the frontend.
    virtual void push(std::string_view event, std::string json_body) = 0;
};

} // namespace clew
