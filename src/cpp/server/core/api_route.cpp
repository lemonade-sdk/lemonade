#include "lemon/server/api_route.h"

#include <cmath>
#include <iomanip>
#include <stdexcept>

#include <lemon/utils/aixlog.hpp>

#include "../telemetry.h"
#include "lemon/alias_manager.h"
#include "lemon/error_types.h"
#include "lemon/mcp_tool.h"
#include "lemon/route_decision_response.h"
#include "lemon/router.h"
#include "lemon/server/model_loader.h"
#include "lemon/streaming_proxy.h"

namespace lemon {

using json = nlohmann::json;

namespace {

const char* to_string(Prefixes prefixes) {
    switch (prefixes) {
        case Prefixes::Quad: return "Quad";
        case Prefixes::Internal: return "Internal";
        case Prefixes::Root: return "Root";
    }
    return "";
}

const char* to_string(ArgIn in) {
    switch (in) {
        case ArgIn::JsonBody: return "JsonBody";
        case ArgIn::Query: return "Query";
        case ArgIn::Path: return "Path";
        case ArgIn::Form: return "Form";
    }
    return "";
}

const char* to_string(RequestFormat format) {
    switch (format) {
        case RequestFormat::Raw: return "Raw";
        case RequestFormat::Json: return "Json";
        case RequestFormat::OptionalJson: return "OptionalJson";
        case RequestFormat::Form: return "Form";
    }
    return "";
}

bool is_file_arg(const RouteArg& arg) {
    return arg.schema.value("format", std::string()) == "binary";
}

// The error extract for a buffered generation: the streaming plumbing reports backend
// failures as a payload in the sink, either an SSE-style "data: {\"error\":...}" event
// or the backend's own JSON error body, rather than throwing.
json extract_error_payload(const std::string& buf) {
    std::string body = buf;
    if (body.rfind("data: ", 0) == 0) {
        body = body.substr(6);
    }
    const auto start = body.find_first_not_of(" \t\r\n");
    if (start == std::string::npos || body[start] != '{') {
        return nullptr;
    }
    try {
        auto parsed = json::parse(body.substr(start));
        if (parsed.is_object() && parsed.contains("error")) {
            return parsed;
        }
    } catch (...) {
    }
    return nullptr;
}

bool valid_error_status(int status_code) {
    return status_code >= 400 && status_code <= 599;
}

const char* to_string(Support support) {
    switch (support) {
        case Support::Available: return "available";
        case Support::Partial: return "partial";
        case Support::NotAvailable: return "not_available";
    }
    return "";
}

const char* to_string(ResponseFormat format) {
    switch (format) {
        case ResponseFormat::Json: return "Json";
        case ResponseFormat::JsonLines: return "JsonLines";
        case ResponseFormat::EventStream: return "EventStream";
        case ResponseFormat::Text: return "Text";
        case ResponseFormat::Binary: return "Binary";
        case ResponseFormat::BinaryStream: return "BinaryStream";
        case ResponseFormat::Empty: return "Empty";
    }
    return "";
}

int get_error_status_code(const json& response, int default_status_code) {
    if (!response.contains("error") || !response["error"].is_object()) {
        return default_status_code;
    }

    const auto& error = response["error"];
    if (error.contains("status_code") && error["status_code"].is_number_integer()) {
        int status_code = error["status_code"].get<int>();
        if (valid_error_status(status_code)) {
            return status_code;
        }
    }

    if (error.contains("details") && error["details"].is_object()) {
        const auto& details = error["details"];
        if (details.contains("status_code") && details["status_code"].is_number_integer()) {
            int status_code = details["status_code"].get<int>();
            if (valid_error_status(status_code)) {
                return status_code;
            }
        }
    }

    if (error.contains("type") && error["type"].is_string()) {
        const std::string type = error["type"].get<std::string>();

        if (type == ErrorType::INVALID_REQUEST ||
            type == "invalid_request_error" ||
            type == ErrorType::UNSUPPORTED_OPERATION) {
            return 400;
        }

        if (type == ErrorType::MODEL_NOT_LOADED) {
            return 404;
        }
    }

    return default_status_code;
}

void set_router_residency_conflict_response(const std::exception& error, httplib::Response& res) {
    res.status = 409;
    const json response = {
        {"error", {
            {"message", error.what()},
            {"type", ErrorType::ROUTER_RESIDENCY_CONFLICT},
            {"param", "model"},
            {"code", ErrorType::ROUTER_RESIDENCY_CONFLICT},
        }},
    };
    res.set_content(response.dump(), "application/json");
}

} // namespace

json route_spec_to_json(const RouteSpec& spec) {
    json args = json::array();
    for (const auto& arg : spec.args) {
        args.push_back({
            {"name", arg.name},
            {"in", to_string(arg.in)},
            {"schema", arg.schema},
            {"required", arg.required},
            {"supported", to_string(arg.supported)},
            {"description", arg.description},
        });
    }
    json responses = json::array();
    for (const auto& response : spec.responses) {
        json setup = json::array();
        for (const auto& ref : response.setup) {
            setup.push_back({{"route", ref.route}, {"format", to_string(ref.format)}});
        }
        responses.push_back({
            {"format", to_string(response.format)},
            {"schema", response.schema},
            {"setup", setup},
            {"example", response.example},
        });
    }
    return {
        {"id", spec.id},
        {"methods", spec.methods},
        {"paths", spec.paths},
        {"prefixes", to_string(spec.prefixes)},
        {"summary", spec.summary},
        {"description", spec.description},
        {"notes", spec.notes},
        {"experimental", spec.experimental},
        {"args", args},
        {"responses", responses},
        {"request_format", to_string(spec.request_format)},
        {"model_defaults_to_loaded", spec.model_defaults_to_loaded},
        {"validate_args", spec.validate_args},
        {"quiet_log", spec.quiet_log},
    };
}

void ApiRoute::serve(const RouteSpec& spec, const httplib::Request& http,
                     httplib::Response& res) {
    RouteRequest req{http, spec};

    if (spec.request_format == RequestFormat::Json ||
        spec.request_format == RequestFormat::OptionalJson) {
        if (spec.request_format == RequestFormat::OptionalJson && http.body.empty()) {
            req.body = json::object();
        } else {
            try {
                req.body = json::parse(http.body);
            } catch (const json::exception& e) {
                // Not just parse_error: a numeric literal too large for a double raises
                // out_of_range, which is answered the same way.
                write_invalid_body(req, e, res);
                return;
            }
        }
    }

    if (spec.validate_args && req.body.is_object()) {
        json schema = {{"type", "object"}, {"properties", json::object()},
                       {"required", json::array()}};
        for (const auto& arg : spec.args) {
            if (arg.in != ArgIn::JsonBody) continue;
            schema["properties"][arg.name] = arg.schema;
            if (arg.required) schema["required"].push_back(arg.name);
        }
        try {
            McpTool::validate(req.body, schema);
        } catch (const std::invalid_argument& e) {
            write_openai_error(res, 400, e.what());
            return;
        }
    }

    handle(req, res);
}

void ApiRoute::write_invalid_body(const RouteRequest& req, const std::exception& error,
                                  httplib::Response& res) const {
    if (req.http.body.empty()) {
        write_plain_error(res, 400, "Request body is required but was empty");
        return;
    }
    write_plain_error(res, 400, std::string("Invalid JSON in request body: ") + error.what());
}

void ApiRoute::stream_response(RouteRequest& req, httplib::Response& res, StreamFn fn,
                               const std::string& content_type) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");
    if (req.route_decision) {
        attach_route_header(res, req.route_decision->decision);
    }

    json route_decision_json = req.route_decision
        ? route_decision_to_json(req.route_decision->decision)
        : json(nullptr);
    std::string body = req.body.is_null() ? req.http.body : req.body.dump();
    std::function<bool()> is_closed = req.http.is_connection_closed;

    res.set_chunked_content_provider(
        content_type,
        [body = std::move(body), route_decision_json = std::move(route_decision_json),
         is_closed = std::move(is_closed), fn = std::move(fn)](size_t offset,
                                                                httplib::DataSink& sink) {
            if (offset > 0) {
                return false;
            }
            utils::RequestCancelToken token;
            if (is_closed) {
                token.should_cancel = [is_closed]() { return is_closed(); };
            }
            WrappedServer::RequestCancelScope cancel_scope(token);
            stream_with_route_decision(sink, route_decision_json,
                                       [&body, &fn](httplib::DataSink& route_sink) {
                                           fn(body, route_sink);
                                       });
            return false;
        });
}

void ModelRoute::handle(RouteRequest& req, httplib::Response& res) {
    try {
        if (req.spec.request_format == RequestFormat::Form && !build_form_body(req, res)) {
            return;
        }

        resolve_model_name(ctx_, req.body);
        if (req.body.contains("model") && req.body["model"].is_string()) {
            req.model = req.body["model"].get<std::string>();
        }

        if (!validate(req, res)) {
            return;
        }

        if (!req.body.contains("model")) {
            if (!req.spec.model_defaults_to_loaded) {
                write_openai_error(res, 400, "Missing 'model' field in request");
                return;
            }
            if (!ctx_.router->is_model_loaded()) {
                LOG(ERROR, "Server") << "No model loaded and no model specified in request"
                                     << std::endl;
                write_plain_error(res, 400, "No model loaded and no model specified in request");
                return;
            }
        } else {
            const LoadSpan load_span_name = load_span();
            auto span = load_span_name.kind.empty()
                ? nullptr
                : telemetry::TelemetryTracker::start_span(load_span_name.kind, load_span_name.name,
                                                          req.model, req.body);
            try {
                ctx_.model_loader->ensure_loaded(req.model, ModelLoader::load_options(req.body));
            } catch (const std::exception& e) {
                LOG(ERROR, "Server") << "Failed to load model: " << e.what() << std::endl;
                ctx_.model_loader->write_load_error(res, req.model, e.what());
                if (span) span->end_with_error(e.what());
                return;
            }
            if (span) span->cancel();
        }

        run(req, res);
    } catch (const std::exception& e) {
        write_exception(e, res);
    }
}

void ModelRoute::write_exception(const std::exception& error, httplib::Response& res) const {
    if (dynamic_cast<const RouterResidencyConflictException*>(&error)) {
        LOG(WARNING, "Server") << "Router residency conflict: " << error.what() << std::endl;
        set_router_residency_conflict_response(error, res);
        return;
    }
    LOG(ERROR, "Server") << "Request failed: " << error.what() << std::endl;
    write_plain_error(res, 500, error.what());
}

bool ModelRoute::build_form_body(RouteRequest& req, httplib::Response& res) {
    if (!req.http.is_multipart_form_data()) {
        write_openai_error(res, 400, "Request must be multipart/form-data");
        return false;
    }
    req.body = json::object();
    for (const auto& arg : req.spec.args) {
        if (arg.in != ArgIn::Form || is_file_arg(arg) || !req.http.form.has_field(arg.name)) {
            continue;
        }
        const std::string& value = req.http.form.get_field(arg.name);
        const std::string type = arg.schema.value("type", std::string("string"));
        try {
            size_t pos = 0;
            if (type == "integer") {
                const int parsed = std::stoi(value, &pos);
                if (pos != value.size()) throw std::invalid_argument("trailing characters");
                req.body[arg.name] = parsed;
            } else if (type == "number") {
                const float parsed = std::stof(value, &pos);
                if (pos != value.size()) throw std::invalid_argument("trailing characters");
                if (std::isnan(parsed) || std::isinf(parsed)) {
                    throw std::invalid_argument("nan/inf not allowed");
                }
                req.body[arg.name] = parsed;
            } else {
                req.body[arg.name] = value;
            }
        } catch (const std::exception&) {
            write_openai_error(res, 400,
                               "Invalid value for '" + arg.name + "': must be " +
                                   (type == "integer" ? "an integer" : "a number"));
            return false;
        }
    }
    return true;
}

std::string strip_latest_tag(const std::string& name) {
    const std::string suffix = ":latest";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return name.substr(0, name.size() - suffix.size());
    }
    return name;
}

void resolve_model_name(ServerContext& ctx, json& request, bool strip_latest) {
    if (!request.is_object()) return;
    auto resolve = [&ctx](const std::string& name) {
        if (auto target = ctx.alias_manager->resolve_alias(name)) {
            return *target;
        }
        return name;
    };
    if (request.contains("model") && request["model"].is_string()) {
        std::string name = request["model"].get<std::string>();
        request["model"] = resolve(strip_latest ? strip_latest_tag(name) : name);
    }
    if (request.contains("model_name") && request["model_name"].is_string()) {
        request["model_name"] = resolve(request["model_name"].get<std::string>());
    }
}

std::unique_ptr<WrappedServer::RequestCancelScope> cancel_on_disconnect(const RouteRequest& req) {
    utils::RequestCancelToken token;
    if (req.http.is_connection_closed) {
        token.should_cancel = [is_closed = req.http.is_connection_closed]() {
            return is_closed && is_closed();
        };
    }
    return std::make_unique<WrappedServer::RequestCancelScope>(token);
}

void attach_route_decision(json& response, httplib::Response& res,
                           const std::optional<RouterDispatchResult>& decision) {
    if (!decision) {
        return;
    }
    response["x_lemonade_route"] = route_decision_to_json(decision->decision);
    attach_route_header(res, decision->decision);
}

void ApiRoute::record_usage(const json& response, const RouteRequest& req) const {
    if (!response.is_object() ||
        (!response.contains("timings") && !response.contains("usage"))) {
        return;
    }

    StreamingProxy::TelemetryData telemetry = StreamingProxy::extract_telemetry(response);
    std::string model_name = req.body.value("model", "");
    LOG(INFO, "Telemetry") << "Inference completed: model=" << model_name
                           << ", tokens=" << (telemetry.input_tokens + telemetry.output_tokens)
                           << " (in=" << telemetry.input_tokens
                           << ", out=" << telemetry.output_tokens << ")"
                           << ", ttft=" << std::fixed << std::setprecision(2)
                           << telemetry.time_to_first_token << "s"
                           << ", tps=" << telemetry.tokens_per_second << std::endl;

    ctx_.router->update_request_telemetry(model_name, telemetry);
}

void set_error_response(const json& response, httplib::Response& res, int default_status_code) {
    res.status = get_error_status_code(response, default_status_code);
    res.set_content(response.dump(), "application/json");
}

void serve_media_or_error(httplib::Response& res, const std::string& mime_type,
                          const std::function<void(httplib::DataSink&)>& generate) {
    std::string buf;
    httplib::DataSink sink;
    sink.write = [&buf](const char* data, size_t len) { buf.append(data, len); return true; };
    sink.is_writable = []() { return true; };
    sink.done = []() {};
    generate(sink);
    if (buf.empty()) {
        res.status = 502;
        res.set_content(json{{"error", {
            {"message", "Generation failed: the backend produced no output (it likely crashed or ran "
                        "out of GPU memory). Check the server logs."},
            {"type", "backend_error"}}}}.dump(), "application/json");
        return;
    }
    if (auto error_payload = extract_error_payload(buf); !error_payload.is_null()) {
        res.status = get_error_status_code(error_payload, 500);
        res.set_content(error_payload.dump(), "application/json");
        return;
    }
    res.set_content(buf, mime_type);
}

bool write_sse_event(httplib::DataSink& sink, const std::string& event, const json& data) {
    std::string payload = "event: " + event + "\ndata: " + data.dump() + "\n\n";
    return sink.write(payload.c_str(), payload.size());
}

const httplib::FormData* find_form_file(const httplib::Request& http,
                                        std::initializer_list<const char*> names) {
    for (const auto& [field, file] : http.form.files) {
        for (const char* name : names) {
            if (field == name) {
                return &file;
            }
        }
    }
    return nullptr;
}

void write_openai_error(httplib::Response& res, int status, const std::string& message,
                        const std::string& type, const std::string& code) {
    json error = {{"message", message}, {"type", type}};
    if (!code.empty()) {
        error["code"] = code;
    }
    res.status = status;
    res.set_content(json{{"error", error}}.dump(), "application/json");
}

void write_plain_error(httplib::Response& res, int status, const std::string& message) {
    res.status = status;
    res.set_content(json{{"error", message}}.dump(), "application/json");
}

} // namespace lemon
