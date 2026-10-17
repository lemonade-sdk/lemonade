// ApiRoute::serve() argument checks and RouteRequest's argument accessors: how an argument
// sent under several spellings is read, how a drop_invalid argument's bad value is ignored,
// how query and path values are typed, and how a client's model name is canonicalized.

#include "lemon/alias_manager.h"
#include "lemon/server/api_route.h"

#include <cstdio>
#include <regex>
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

const lemon::RouteArg kModel{"model", lemon::ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true,
                             lemon::Support::Available, "Model.", {"name"}};
const lemon::RouteArg kCtxSize{"ctx_size", lemon::ArgIn::JsonBody, {{"type", "integer"}}, false,
                               lemon::Support::Available, "Context size.", {}, false, true};

class CaptureRoute : public lemon::ApiRoute {
public:
    using ApiRoute::ApiRoute;

    lemon::RouteSpec spec() const override {
        lemon::RouteSpec s;
        s.id = "test.capture";
        s.methods = {"POST"};
        s.paths = {"capture"};
        s.args = {kModel, kCtxSize};
        s.request_format = lemon::RequestFormat::Json;
        return s;
    }

    void handle(lemon::RouteRequest& req, httplib::Response& res) override {
        body = req.body;
        model = req.value(kModel, "");
        has_ctx_size = req.has(kCtxSize);
        ctx_size = req.value(kCtxSize, 0);
        res.status = 200;
    }

    json body;
    std::string model;
    bool has_ctx_size = false;
    int ctx_size = 0;
};

const lemon::RouteArg kId{"id", lemon::ArgIn::Path, {{"type", "string"}}, true, lemon::Support::Available, "Id."};
const lemon::RouteArg kLimit{"limit", lemon::ArgIn::Query, {{"type", "integer"}}, false,
                             lemon::Support::Available, "Limit."};
const lemon::RouteArg kVerbose{"verbose", lemon::ArgIn::Query, {{"type", "boolean"}}, false,
                               lemon::Support::Available, "Verbose."};
const lemon::RouteArg kQuery{"query", lemon::ArgIn::Query, {{"type", "string"}}, false,
                             lemon::Support::Available, "Query.", {"q"}};

class ParamsRoute : public lemon::ApiRoute {
public:
    using ApiRoute::ApiRoute;

    lemon::RouteSpec spec() const override {
        lemon::RouteSpec s;
        s.id = "test.params";
        s.methods = {"GET"};
        s.paths = {"items/{id}"};
        s.args = {kId, kLimit, kVerbose, kQuery};
        return s;
    }

    void handle(lemon::RouteRequest& req, httplib::Response& res) override {
        params = req.params;
        id = req.value(kId, "");
        limit = req.arg(kLimit);
        verbose = req.value(kVerbose, false);
        query = req.value(kQuery, "");
        res.status = 200;
    }

    json params;
    std::string id;
    json limit;
    bool verbose = false;
    std::string query;
};

void serve(CaptureRoute& route, const std::string& request_body, int& status) {
    httplib::Request http;
    http.body = request_body;
    httplib::Response res;
    route.serve(route.spec(), http, res);
    status = res.status;
}

void expect_model(const std::string& request_body, const std::string& model) {
    lemon::ServerContext ctx;
    CaptureRoute route(ctx);
    int status = 0;
    serve(route, request_body, status);
    check(status == 200 && route.model == model && !route.body.contains("name"),
          request_body + " reads model '" + model + "'");
}

int status_of(const std::string& request_body) {
    lemon::ServerContext ctx;
    CaptureRoute route(ctx);
    int status = 0;
    serve(route, request_body, status);
    return status;
}

void serve_params(ParamsRoute& route, const std::string& path,
                  std::initializer_list<std::pair<std::string, std::string>> query) {
    httplib::Request http;
    http.path = path;
    std::regex_match(http.path, http.matches, std::regex("/items/([^/]+)"));
    for (const auto& [name, value] : query) {
        http.params.emplace(name, value);
    }
    httplib::Response res;
    route.serve(route.spec(), http, res);
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

    check(status_of(R"({"model": "", "name": ""})") == 400, "empty model and name answer 400");
    check(status_of(R"({"model": 5})") == 400, "a non-string model answers 400");
    check(status_of(R"({})") == 400, "a missing model answers 400");

    {
        lemon::ServerContext ctx;
        CaptureRoute route(ctx);
        int status = 0;
        serve(route, R"({"model": "a", "ctx_size": "4096"})", status);
        check(status == 200 && !route.has_ctx_size && !route.body.contains("ctx_size"),
              "a drop_invalid ctx_size that is not an integer is ignored");
        serve(route, R"({"model": "a", "ctx_size": 4096})", status);
        check(status == 200 && route.has_ctx_size && route.ctx_size == 4096,
              "a valid ctx_size is read through its constant");
    }

    {
        lemon::ServerContext ctx;
        ParamsRoute route(ctx);
        serve_params(route, "/items/abc", {{"limit", "5"}, {"verbose", "true"}, {"q", "cats"}});
        check(route.id == "abc", "a path argument is read by its name");
        check(route.limit == 5, "an integer query value is an integer");
        check(route.verbose, "a boolean query value of true is true");
        check(route.query == "cats" && !route.params.contains("q"),
              "a query alias is read under the argument's name");

        serve_params(route, "/items/abc", {{"limit", "5x"}, {"verbose", "yes"}});
        check(route.limit == "5x", "a query value that is not an integer stays a string");
        check(!route.verbose, "a boolean query value other than true is false");
        check(route.query.empty() && !route.params.contains("query"), "an absent query argument is absent");
    }

    {
        lemon::AliasManager aliases;
        std::string error;
        aliases.set_alias("nickname", "Qwen3-0.6B-GGUF", error);
        lemon::ServerContext ctx;
        ctx.alias_manager = &aliases;
        check(lemon::canonical_model_name(ctx, "Qwen3-0.6B-GGUF:latest") == "Qwen3-0.6B-GGUF",
              "canonical_model_name strips :latest");
        check(lemon::canonical_model_name(ctx, "nickname:latest") == "Qwen3-0.6B-GGUF",
              "canonical_model_name follows an alias after stripping :latest");
        check(lemon::canonical_model_name(ctx, "Qwen3-0.6B-GGUF:latest", false) == "Qwen3-0.6B-GGUF:latest",
              "canonical_model_name keeps :latest when asked to");
    }

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
