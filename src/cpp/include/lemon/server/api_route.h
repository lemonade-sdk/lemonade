#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/router_dispatch.h"
#include "lemon/server/server_context.h"
#include "lemon/wrapped_server.h"

namespace lemon {

enum class Prefixes { Quad, Internal, Root };      // URLs and auth; see RouteRegistry
enum class ArgIn { JsonBody, Query, Path, Form };  // where an argument comes from
enum class RequestFormat { Raw, Json, OptionalJson, Form };  // how ApiRoute parses the request body

// Clients parse each kind of body differently, and some stream.
enum class ResponseFormat {
    Json,          // one JSON body
    JsonLines,     // newline-delimited JSON objects, streamed (application/x-ndjson)
    EventStream,   // server-sent events with JSON data, streamed (text/event-stream)
    Text,          // e.g. Prometheus metrics, markdown, a transcript
    Binary,        // e.g. audio, a glTF mesh
    BinaryStream,  // binary, streamed as generated, e.g. speech
    Empty,         // e.g. the 202 answering an MCP notification
};

// How much of an argument Lemonade honors; the docs give each argument a badge for it.
enum class Support { Available, Partial, NotAvailable };

struct RouteArg {
    std::string name;
    ArgIn in;
    nlohmann::json schema;            // JSON-schema subset
    bool required = false;
    Support supported = Support::Available;  // Partial: the description states the limit
    std::string description;
};

// Some examples require lemond to be in a certain state, for example unload requires a loaded
// model. ExampleRef allows one example to take a dependence on another to achieve that state.
struct ExampleRef {
    std::string route;                // RouteSpec::id
    ResponseFormat format;            // selects which of the route's examples
};

// Each format a route returns gets its own schema and example in the docs.
struct RouteResponse {
    ResponseFormat format;

    // JSON formats only. JSON Schema for the body, each NDJSON line, or each SSE event's data;
    // --check validates against it. A format with several shapes, such as /load's three, uses oneOf.
    nlohmann::json schema;

    // Examples to run first, in order, when generating the documentation. Examples can reference
    // their dependences' responses, for example {"id": "$lemonade.jobs_create/id"}.
    std::vector<ExampleRef> setup;

    // A request producing this format; gen_api_boilerplate.py records its response into the docs
    // and --check validates it.
    nlohmann::json example;
};

struct RouteSpec {
    std::string id;                   // "<page>.<name>", e.g. "openai.chat_completions"; docs anchor
    std::vector<std::string> methods; // e.g. {"POST"}
    std::vector<std::string> paths;   // without prefix; extra entries are aliases, e.g. rerank, reranking
    Prefixes prefixes = Prefixes::Quad;
    std::string summary;              // one line in the page's summary table
    std::string description;          // reference text for the route's docs section
    std::vector<std::string> notes;
    bool experimental = false;        // badged as experimental in the docs
    std::vector<RouteArg> args;       // every argument, documented whether or not validated
    std::vector<RouteResponse> responses;  // one per format; empty for an unsupported route, e.g. Ollama's POST /api/create (501)
    RequestFormat request_format = RequestFormat::Raw;  // Raw: no parsing; OptionalJson: an empty body is {}
    bool model_defaults_to_loaded = false;  // ModelRoute only; see ModelRoute below
    bool validate_args = false;       // check args with McpTool's validator; off for pass-through routes
    bool quiet_log = false;           // leave out of the access log
};

nlohmann::json route_spec_to_json(const RouteSpec& spec);

// One request's data, built by ApiRoute::serve() and handed to handle().
struct RouteRequest {
    const httplib::Request& http;
    const RouteSpec& spec;
    nlohmann::json body;              // parsed unless spec.request_format is Raw
    std::string model;                // ModelRoute: the resolved model name
    std::optional<RouterDispatchResult> route_decision;
};

class ApiRoute {
public:
    explicit ApiRoute(ServerContext& ctx) : ctx_(ctx) {}
    virtual ~ApiRoute() = default;
    virtual RouteSpec spec() const = 0;
    virtual void handle(RouteRequest& req, httplib::Response& res) = 0;

    // RouteRegistry calls this for every request: it parses the body as spec.request_format
    // says, then calls handle().
    void serve(const RouteSpec& spec, const httplib::Request& http, httplib::Response& res);

    using StreamFn = std::function<void(const std::string& body, httplib::DataSink& sink)>;

    // The only way a route streams. fn runs after handle() returns, so this carries the router
    // decision and the cancel token into the stream with it. fn receives req.body serialized.
    static void stream_response(RouteRequest& req, httplib::Response& res, StreamFn fn,
                                const std::string& content_type = "text/event-stream");

protected:
    // Clients parse each route's invalid-JSON error, and the shapes differ by route. The
    // default is a 400 with a plain-string error; a route with another shape overrides it.
    virtual void write_invalid_body(const RouteRequest& req, const std::exception& error,
                                    httplib::Response& res) const;

    // Records token usage and timings for telemetry.
    void record_usage(const nlohmann::json& response, const RouteRequest& req) const;

    ServerContext& ctx_;
};

class ModelRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;
    void handle(RouteRequest& req, httplib::Response& res) final;  // resolve, validate, load, run

protected:
    virtual bool validate(RouteRequest& req, httplib::Response& res) { return true; }
    virtual void run(RouteRequest& req, httplib::Response& res) = 0;

    // An exception escaping validate() or run() lands here. The default answers a residency
    // conflict with 409 and anything else with a 500 plain-string error; a route whose clients
    // parse another shape overrides it.
    virtual void write_exception(const std::exception& error, httplib::Response& res) const;

    // Telemetry consumers match a failed auto-load's span by these names, which predate route
    // ids. A route that recorded no span returns an empty kind.
    struct LoadSpan {
        std::string kind;
        std::string name;
    };
    virtual LoadSpan load_span() const { return {}; }

private:
    bool build_form_body(RouteRequest& req, httplib::Response& res);
};

// Clients name models by aliases and, following Ollama, with a ":latest" tag; this
// rewrites "model" and "model_name" in request to the name the registry knows.
void resolve_model_name(ServerContext& ctx, nlohmann::json& request, bool strip_latest = true);
std::string strip_latest_tag(const std::string& name);

// Aborts the upstream backend request when the client disconnects during a non-streaming
// request (Invariant #11), for as long as the returned scope lives.
std::unique_ptr<WrappedServer::RequestCancelScope> cancel_on_disconnect(const RouteRequest& req);

// Adds the router decision to the response body and headers.
void attach_route_decision(nlohmann::json& response, httplib::Response& res,
                           const std::optional<RouterDispatchResult>& decision);

// Maps a backend error body to its HTTP status.
void set_error_response(const nlohmann::json& response, httplib::Response& res,
                        int default_status_code = 500);

// A generation streamed straight to the client commits a 200 before the backend runs, so a
// backend that crashes or runs out of memory would look like an empty successful file. This
// buffers the output and answers with an error instead.
void serve_media_or_error(httplib::Response& res, const std::string& mime_type,
                          const std::function<void(httplib::DataSink&)>& generate);

// Writes one named server-sent event. Returns false when the client has disconnected.
bool write_sse_event(httplib::DataSink& sink, const std::string& event,
                     const nlohmann::json& data);

// The one form file a route accepts under any of names, or nullptr.
const httplib::FormData* find_form_file(const httplib::Request& http,
                                        std::initializer_list<const char*> names);

void write_openai_error(httplib::Response& res, int status, const std::string& message,
                        const std::string& type = "invalid_request_error",
                        const std::string& code = "");
void write_plain_error(httplib::Response& res, int status, const std::string& message);

} // namespace lemon
