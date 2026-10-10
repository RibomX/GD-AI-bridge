// Local HTTP server: receives JSON requests, runs them on the game's main thread.
// httplib must come before Geode so winsock2.h wins over winsock.h.
#include <httplib.h>

#include "Api.hpp"

#include <Geode/Geode.hpp>
#include <atomic>
#include <future>
#include <thread>

using namespace geode::prelude;

namespace {
    httplib::Server* g_server = nullptr;
    int g_port = 0;

    // Run `fn` on the cocos main thread and wait for its result.
    // Every editor call must go through here: cocos2d is not thread-safe.
    // GD stops its main loop while minimized, so a request can time out; it is
    // then cancelled instead of silently running later when the window returns.
    ApiResult runOnMain(std::function<ApiResult()> fn) {
        auto promise = std::make_shared<std::promise<ApiResult>>();
        auto future = promise->get_future();
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        queueInMainThread([promise, cancelled, fn = std::move(fn)] {
            if (cancelled->exchange(true)) return;
            try {
                promise->set_value(fn());
            } catch (std::exception const& e) {
                promise->set_value(ApiResult::error(500, e.what()));
            }
        });
        if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
            // If the main thread already picked it up, the result is coming; wait for it.
            if (cancelled->exchange(true)) return future.get();
            return ApiResult::error(504, "Game did not respond in time, nothing was changed. Is the game window minimized?");
        }
        return future.get();
    }

    // Only accept requests from local programs, not from web pages:
    // browsers always send Origin on cross-site requests, and a foreign
    // Host header means DNS rebinding.
    bool isTrustedRequest(httplib::Request const& req) {
        if (req.has_header("Origin")) return false;
        auto host = req.get_header_value("Host");
        auto port = fmt::format(":{}", g_port);
        return host == "127.0.0.1" + port || host == "localhost" + port;
    }

    void send(httplib::Response& res, ApiResult const& result) {
        res.status = result.status;
        res.set_content(result.body.dump(matjson::NO_INDENTATION), "application/json");
    }

    using Handler = ApiResult (*)(matjson::Value const& input);

    void route(httplib::Server& server, char const* method, char const* path, Handler handler) {
        auto fn = [handler](httplib::Request const& req, httplib::Response& res) {
            if (!isTrustedRequest(req)) {
                return send(res, ApiResult::error(403, "Requests from browsers or foreign hosts are not allowed"));
            }

            // GET: query params become the input object. POST: JSON body.
            matjson::Value input = matjson::Value::object();
            if (req.method == "GET") {
                for (auto const& [key, value] : req.params) input[key] = value;
            } else if (!req.body.empty()) {
                auto parsed = matjson::parse(req.body);
                if (!parsed) {
                    return send(res, ApiResult::error(400, "Invalid JSON: " + parsed.unwrapErr().message));
                }
                input = std::move(parsed).unwrap();
            }

            send(res, runOnMain([handler, input = std::move(input)] { return handler(input); }));
        };
        if (std::string_view(method) == "GET") server.Get(path, fn);
        else server.Post(path, fn);
    }
}

void startServer(int port) {
    g_port = port;
    g_server = new httplib::Server();
    auto& server = *g_server;

    server.Get("/", [](httplib::Request const& req, httplib::Response& res) {
        if (!isTrustedRequest(req)) return send(res, ApiResult::error(403, "Forbidden"));
        send(res, ApiResult::ok(apiInfo()));
    });

    for (auto const& r : apiRoutes()) {
        route(server, r.method, r.path, r.handler);
    }

    std::thread([port] {
        log::info("API server listening on http://127.0.0.1:{}", port);
        if (!g_server->listen("127.0.0.1", port)) {
            log::error("Could not start API server on port {} (already in use?)", port);
        }
    }).detach();
}
