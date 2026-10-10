#pragma once

#include <matjson.hpp>
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

void startServer(int port);
