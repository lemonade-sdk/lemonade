#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/route_arg.h"
#include "lemon/server/route_error.h"
#include "lemon/server/router_dispatch.h"
#include "lemon/server/server_context.h"
#include "lemon/wrapped_server.h"

namespace lemon {

enum class Prefixes { Quad, Internal, Root };      // URLs and auth; see RouteRegistry
enum class RequestFormat { Raw, Json, OptionalJson, Form };  // how ApiRoute parses the request body
enum class ErrorFormat { Plain, OpenAi, Anthropic };          // the body shape of the base class's error responses

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

// Each format a route returns gets its own schema in the docs.
struct RouteResponse {
    ResponseFormat format;

    // JSON formats only. JSON Schema for the body, each NDJSON line, or each SSE event's data;
    // --check validates against it. A format with several shapes, such as /load's three, uses oneOf.
    // Ordered, so the docs list the fields as written rather than alphabetically.
    nlohmann::ordered_json schema;
};

// Some examples require lemond to be in a certain state, for example unload requires a loaded
// model. A step sends a request that puts lemond in that state before the example runs.
struct ExampleStep {
    std::string route;                // RouteSpec::id
    std::string example;              // that route's RouteExample::name; runs after its own setup
    nlohmann::ordered_json args;      // set instead of example: a request sent as is and left out of the docs
};

// gen_api_boilerplate.py sends each example to lemond and records the response into the docs, and
// --check validates the response against the schema of the example's format.
struct RouteExample {
    std::string name;                 // subsection heading, e.g. "streaming" or "save"; required when a route has several
    std::string description;          // what this example shows that the route's other text does not; opens its subsection
    ResponseFormat format;            // the RouteResponse the response is validated against
    nlohmann::ordered_json args;      // argument values, e.g. {"id": "$lemonade.jobs_create/id"}; ordered so the docs show them as written
    std::vector<ExampleStep> setup;   // run first, in order
};

struct RouteSpec {
    std::string id;                   // "<page>.<name>", e.g. "openai.chat_completions"; names the generated docs region
    std::vector<std::string> methods; // e.g. {"POST"}
    std::vector<std::string> paths;   // without prefix; extra entries are aliases, e.g. rerank, reranking
    Prefixes prefixes = Prefixes::Quad;
    std::string summary;              // one line in the page's summary table; empty when hand-written rows replace it
    std::string modality;             // the summary table's Modality cell, e.g. "messages -> completion"
    std::string description;          // reference text for the route's docs section
    std::vector<std::string> notes;   // sections the docs place after Parameters, such as an actions table
    bool experimental = false;        // badged as experimental in the docs
    std::vector<RouteArg> args;       // every argument
    std::vector<RouteResponse> responses;  // one per format; empty for an unsupported route, e.g. Ollama's POST /api/create (501)
    std::vector<RouteExample> examples;    // the first is the route's main example
    std::vector<RouteError> errors;        // every error the route writes, besides those its base class adds
    ErrorFormat error_format = ErrorFormat::Plain;
    RequestFormat request_format = RequestFormat::Raw;  // Raw: no body parsing, only query argument values are checked; OptionalJson: an empty or null body is {}
    bool quiet_log = false;           // leave out of the access log
};

nlohmann::ordered_json route_spec_to_json(const RouteSpec& spec);

// One request's data, built by ApiRoute::serve() and handed to handle().
struct RouteRequest {
    const httplib::Request& http;
    const RouteSpec& spec;
    nlohmann::json body;              // parsed unless spec.request_format is Raw
    nlohmann::json params = nlohmann::json::object();  // Query and Path arguments, converted to their schema types
    std::string model;                // ModelRoute: the resolved model name
    // Set by validate() when it picked an already-loaded model: loading it again with the
    // default purpose would demote a routing helper to a standard model.
    bool model_already_loaded = false;
    std::optional<RouterDispatchResult> route_decision;

    // serve() has renamed every alias to the argument's name, so these read one place: the body
    // for JsonBody and Form arguments, params for Query and Path ones.
    bool has(const RouteArg& arg) const;
    const nlohmann::json& arg(const RouteArg& arg) const;  // null when the argument was not sent
    template <typename T>
    T value(const RouteArg& arg, const T& fallback) const {
        return has(arg) ? this->arg(arg).template get<T>() : fallback;
    }
    std::string value(const RouteArg& arg, const char* fallback) const {
        return value<std::string>(arg, fallback);
    }
    const httplib::FormData* file(const RouteArg& arg) const;  // a Form file argument, or nullptr
};

class ApiRoute {
public:
    explicit ApiRoute(ServerContext& ctx) : ctx_(ctx) {}
    virtual ~ApiRoute() = default;
    virtual RouteSpec spec() const = 0;
    virtual void handle(RouteRequest& req, httplib::Response& res) = 0;

    static const RouteError kBodyNotJson;
    static const RouteError kArgMissing;
    static const RouteError kArgInvalid;
    static const RouteError kUnexpectedException;

    // RouteRegistry calls this for every request: it parses the body as spec.request_format
    // says, checks the arguments, then calls handle().
    void serve(const RouteSpec& spec, const httplib::Request& http, httplib::Response& res);

    using StreamFn = std::function<void(const std::string& body, httplib::DataSink& sink)>;

    // The only way a route streams. fn runs after handle() returns, so this carries the router
    // decision and the cancel token into the stream with it. fn receives req.body serialized.
    static void stream_response(RouteRequest& req, httplib::Response& res, StreamFn fn,
                                const std::string& content_type = "text/event-stream");

protected:
    void record_usage(const nlohmann::json& response, const RouteRequest& req) const;

    bool check_args(RouteRequest& req, httplib::Response& res, bool query_only = false) const;

    ServerContext& ctx_;
};

class ModelRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;
    void handle(RouteRequest& req, httplib::Response& res) final;  // resolve, validate, load, run

    static const RouteError kFormNotMultipart;
    static const RouteError kModelMissing;

protected:
    virtual const RouteArg& model_arg() const = 0;  // the argument that names the model
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

// Clients name models by aliases and, following Ollama, with a ":latest" tag; this returns
// the name the registry knows. A registered model or alias may itself end in ":latest".
std::string canonical_model_name(ServerContext& ctx, const std::string& name, bool strip_latest = true);
std::string strip_latest_tag(const std::string& name);

// Aborts the upstream backend request when the client disconnects during a non-streaming
// request, for as long as the returned scope lives.
std::unique_ptr<WrappedServer::RequestCancelScope> cancel_on_disconnect(const RouteRequest& req);

// Adds the router decision to the response body and headers.
void attach_route_decision(nlohmann::json& response, httplib::Response& res,
                           const std::optional<RouterDispatchResult>& decision);

extern const RouteError kBackendError;
extern const RouteError kMalformedBackendResponse;

// Fields a route forwards without declaring them, such as a model definition's, are not
// checked by the validator, so the step that reads one answers a wrong type with this.
extern const RouteError kUnlistedFieldInvalid;
extern const RouteError kModelUnloaded;

// Maps a backend error body to its HTTP status.
void set_error_response(const nlohmann::json& response, httplib::Response& res,
                        const RouteError& fallback);

extern const RouteError kNoOutputError;

// A generation streamed straight to the client commits a 200 before the backend runs, so a
// backend that crashes or runs out of memory would look like an empty successful file. This
// buffers the output and answers with an error instead.
void serve_media_or_error(httplib::Response& res, const std::string& mime_type,
                          const std::function<void(httplib::DataSink&)>& generate);

// Writes one named server-sent event. Returns false when the client has disconnected.
bool write_sse_event(httplib::DataSink& sink, const std::string& event,
                     const nlohmann::json& data);

std::vector<RouteError> join_errors(std::initializer_list<std::vector<RouteError>> lists);

void write_openai_error(httplib::Response& res, const RouteError& error, const std::string& message,
                        const std::string& type = "invalid_request_error");
void write_plain_error(httplib::Response& res, const RouteError& error, const std::string& message);
void write_anthropic_error(httplib::Response& res, const RouteError& error, const std::string& message);
void write_error(httplib::Response& res, ErrorFormat format, const RouteError& error,
                 const std::string& message);

} // namespace lemon
