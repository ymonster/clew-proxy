#include "transport/route_registry.hpp"

#include <chrono>
#include <string>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/auth_middleware.hpp"
#include "common/api_context.hpp"
#include "common/api_exception.hpp"
#include "core/log.hpp"

namespace clew {

namespace {

// DESIGN appendix: mandatory three-tier dispatch in every handler.
//
// Every request produces at least one log line at INFO level covering the
// method, path, final status, and duration. Any error path (auth, known
// api_exception, unexpected std::exception, unknown throw) is logged at
// WARN/ERROR with the exception message attached so the last seen route +
// failure mode is always in clew.log before anything can crash.
void dispatch(route_handler_fn handler,
              auth_policy policy,
              auth_middleware& mw,
              const httplib::Request& req,
              httplib::Response& res,
              api_context& ctx) {
    const auto t0 = std::chrono::steady_clock::now();

    auto elapsed_us = [&]() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    };

    if (policy == auth_policy::required && !mw.verify(req)) {
        res.status = 401;
        res.set_content(R"({"error":"Unauthorized"})", "application/json");
        PC_LOG_WARN("[api] {} {} -> 401 unauthorized ({}us)",
                    req.method, req.path, elapsed_us());
        return;
    }

    try {
        handler(req, res, ctx);
        PC_LOG_INFO("[api] {} {} -> {} ({}us)",
                    req.method, req.path, res.status, elapsed_us());
    } catch (const api_exception& e) {
        res.status = api_error_to_http_status(e.code());
        nlohmann::json body;
        body["error"] = e.message();
        if (!e.details().is_null() && !e.details().empty()) {
            body["details"] = e.details();
        }
        res.set_content(body.dump(), "application/json");
        PC_LOG_WARN("[api] {} {} -> {} api_exception: {} ({}us)",
                    req.method, req.path, res.status, e.message(), elapsed_us());
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(R"({"error":"internal error"})", "application/json");
        PC_LOG_ERROR("[api] {} {} -> 500 std::exception: {} ({}us)",
                     req.method, req.path, e.what(), elapsed_us());
    } catch (...) {
        res.status = 500;
        res.set_content(R"({"error":"internal error"})", "application/json");
        PC_LOG_ERROR("[api] {} {} -> 500 non-std exception ({}us)",
                     req.method, req.path, elapsed_us());
    }
}

} // namespace

route_registry::route_registry(httplib::Server& server, api_context& ctx, auth_middleware& mw)
    : server_(server), ctx_(ctx), mw_(mw) {}

void route_registry::add(const route_def& def) {
    std::string pattern{def.pattern};
    route_handler_fn handler = def.handler;
    auth_policy policy       = def.auth;

    // Capture references to the external api_context / auth_middleware (owned
    // by main.cpp) instead of capturing `this`. route_registry is usually a
    // short-lived stack object in the server's constructor, so capturing its
    // `this` would leave the adapter lambda with a dangling pointer for the
    // rest of the server's lifetime.
    api_context&     ctx_ref = ctx_;
    auth_middleware& mw_ref  = mw_;

    auto adapter = [handler, policy, &ctx_ref, &mw_ref](const httplib::Request& req, httplib::Response& res) {
        dispatch(handler, policy, mw_ref, req, res, ctx_ref);
    };

    switch (def.method) {
        case http_method::get:     server_.Get(pattern,    adapter); break;
        case http_method::post:    server_.Post(pattern,   adapter); break;
        case http_method::put:     server_.Put(pattern,    adapter); break;
        case http_method::delete_: server_.Delete(pattern, adapter); break;
    }
}

} // namespace clew
