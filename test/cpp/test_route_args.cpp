// ApiRoute::serve() argument checks: how an argument sent under several spellings is
// read, and how a drop_invalid argument's bad value is ignored.

#include "lemon/server/api_route.h"

#include <cstdio>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++failures;
    }
}

class CaptureRoute : public lemon::ApiRoute {
public:
    using ApiRoute::ApiRoute;

    lemon::RouteSpec spec() const override {
        lemon::RouteSpec s;
        s.id = "test.capture";
        s.methods = {"POST"};
        s.paths = {"capture"};
        s.args = {
            {"model", lemon::ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true,
             lemon::Support::Available, "Model.", {"name"}},
            {"ctx_size", lemon::ArgIn::JsonBody, {{"type", "integer"}}, false,
             lemon::Support::Available, "Context size.", {}, false, true},
        };
        s.request_format = lemon::RequestFormat::Json;
        return s;
    }

    void handle(lemon::RouteRequest& req, httplib::Response& res) override {
        body = req.body;
        res.status = 200;
    }

    json body;
};

struct Result {
    int status;
    json body;
};

Result serve(const std::string& request_body) {
    lemon::ServerContext ctx;
    CaptureRoute route(ctx);
    httplib::Request http;
    http.body = request_body;
    httplib::Response res;
    route.serve(route.spec(), http, res);
    return {res.status, route.body};
}

void expect_model(const std::string& request_body, const std::string& model) {
    Result r = serve(request_body);
    check(r.status == 200 && r.body.value("model", "") == model && !r.body.contains("name"),
          request_body + " reads model '" + model + "'");
}

} // namespace

int main() {
    // Ollama's Go client sends both spellings and leaves one empty.
    expect_model(R"({"model": "", "name": "a"})", "a");
    expect_model(R"({"model": "a", "name": ""})", "a");
    expect_model(R"({"name": "a"})", "a");
    // The name comes first, as in Ollama's cmp.Or(model, name), and the unused spelling is not checked.
    expect_model(R"({"model": "a", "name": "b"})", "a");
    expect_model(R"({"model": "a", "name": 5})", "a");
    expect_model(R"({"model": null, "name": "a"})", "a");

    Result empty = serve(R"({"model": "", "name": ""})");
    check(empty.status == 400, "empty model and name answer 400");
    Result wrong_type = serve(R"({"model": 5})");
    check(wrong_type.status == 400, "a non-string model answers 400");
    Result missing = serve(R"({})");
    check(missing.status == 400, "a missing model answers 400");

    Result bad_ctx = serve(R"({"model": "a", "ctx_size": "4096"})");
    check(bad_ctx.status == 200 && !bad_ctx.body.contains("ctx_size"),
          "a drop_invalid ctx_size that is not an integer is ignored");
    Result good_ctx = serve(R"({"model": "a", "ctx_size": 4096})");
    check(good_ctx.status == 200 && good_ctx.body.value("ctx_size", 0) == 4096,
          "a valid ctx_size is kept");

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
