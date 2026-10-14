#pragma once

#include <matjson.hpp>
#include <optional>
#include <string>
#include <vector>

struct ApiResult {
    int status = 200;
    matjson::Value body;

    static ApiResult ok(matjson::Value body) {
        return {200, std::move(body)};
    }
    static ApiResult error(int status, std::string const& message) {
        return {status, matjson::makeObject({{"error", message}})};
    }
};

struct ApiRoute {
    char const* method;
    char const* path;
    char const* description;
    ApiResult (*handler)(matjson::Value const& input);
};

// Endpoints exposed by the server. Handlers run on the main thread.
std::vector<ApiRoute> const& apiRoutes();
matjson::Value apiInfo();

// Automated playtest (Tester.cpp), main thread only. testPoll returns nothing while running.
ApiResult testStart(matjson::Value const& input);
std::optional<ApiResult> testPoll(double timeoutSeconds);

void startServer(int port);
