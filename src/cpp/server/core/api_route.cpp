#include "lemon/server/api_route.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <regex>
#include <stdexcept>

#include <lemon/utils/aixlog.hpp>

#include "../telemetry.h"
#include "lemon/alias_manager.h"
#include "lemon/anthropic_error.h"
#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/route_decision_response.h"
#include "lemon/router.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/shared_args.h"
#include "lemon/streaming_proxy.h"

namespace lemon {

using json = nlohmann::json;

const RouteError kBackendError{
    500, "", "The backend reports an error; the backend's own status replaces this one when it sends one"};
const RouteError kMalformedBackendResponse{
    500, "", "The backend's response lacks a field the route returns, such as `choices` or `labels`"};
const RouteError kUnlistedFieldInvalid{400, "", "A field not listed above has the wrong JSON type"};
const RouteError kModelUnloaded{404, "", "The model is unloaded while the request waits for it"};
const RouteError ApiRoute::kBodyNotJson{400, "invalid_json", "The body is not a JSON object"};
const RouteError ApiRoute::kArgMissing{400, "missing_argument", "A required argument is missing"};
const RouteError ApiRoute::kArgInvalid{
    400, "invalid_argument", "An argument or a value nested in it has the wrong JSON type, or a value its schema does not list"};
const RouteError ModelRoute::kFormNotMultipart{400, "", "The request is not `multipart/form-data`"};
const RouteError ModelRoute::kModelMissing{400, "", "The request names no `model`"};
const RouteError ApiRoute::kUnexpectedException{
    500, "", "The request fails unexpectedly"};
const RouteError kNoOutputError{
    502, "", "The backend produces no output, for example after it crashes or runs out of GPU memory"};

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

const char* to_string(ErrorFormat format) {
    switch (format) {
        case ErrorFormat::Plain: return "Plain";
        case ErrorFormat::OpenAi: return "OpenAi";
        case ErrorFormat::Anthropic: return "Anthropic";
    }
    return "";
}

bool matches_type(const json& value, const std::string& type) {
    if (type == "string") return value.is_string();
    if (type == "integer") {
        if (value.is_number_integer()) return true;
        if (!value.is_number_float()) return false;
        double number = value.get<double>();
        return std::isfinite(number) && std::floor(number) == number;
    }
    if (type == "number") return value.is_number();
    if (type == "boolean") return value.is_boolean();
    if (type == "array") return value.is_array();
    if (type == "object") return value.is_object();
    if (type == "null") return value.is_null();
    return true;
}

// A query value arrives as a string, so only a numeric or boolean type constrains its spelling.
bool matches_query_type(const std::string& value, const std::string& type) {
    if (type == "integer") {
        return !value.empty() && value.find_first_not_of("0123456789") == std::string::npos;
    }
    if (type == "number") {
        try {
            size_t used = 0;
            double number = std::stod(value, &used);
            return used == value.size() && std::isfinite(number);
        } catch (const std::exception&) {
            return false;
        }
    }
    if (type == "boolean") {
        return value == "true" || value == "false";
    }
    return true;
}

bool matches_schema(const json& value, const json& schema, bool from_query) {
    if (schema.contains("type")) {
        const json& types = schema["type"];
        bool any = false;
        for (const auto& type : types.is_array() ? types : json::array({types})) {
            const std::string name = type.get<std::string>();
            any = any || (from_query ? matches_query_type(value.get<std::string>(), name)
                                     : matches_type(value, name));
        }
        if (!any) return false;
    }
    if (schema.contains("enum")) {
        const json& values = schema["enum"];
        if (std::find(values.begin(), values.end(), value) == values.end()) return false;
    }
    if (schema.contains("const") && value != schema["const"]) {
        return false;
    }
    if (value.is_number() || (from_query && (schema.contains("minimum") || schema.contains("maximum")))) {
        const double number = value.is_number() ? value.get<double>()
                                                : std::stod(value.get<std::string>());
        if (schema.contains("minimum") && number < schema["minimum"].get<double>()) return false;
        if (schema.contains("maximum") && number > schema["maximum"].get<double>()) return false;
    }
    if (value.is_string() && !from_query) {
        const std::string& text = value.get_ref<const std::string&>();
        if (schema.contains("minLength") && text.size() < schema["minLength"].get<size_t>()) {
            return false;
        }
        if (schema.contains("maxLength") && text.size() > schema["maxLength"].get<size_t>()) {
            return false;
        }
        if (schema.contains("pattern") &&
            !std::regex_search(text, std::regex(schema["pattern"].get<std::string>()))) {
            return false;
        }
    }
    return true;
}

// The schema that value or one of its nested values fails, with path extended to that value.
const json* find_mismatch(const json& value, const json& schema, bool from_query,
                          std::string& path) {
    if (!matches_schema(value, schema, from_query)) return &schema;
    if (value.is_array() && schema.contains("items")) {
        for (size_t i = 0; i < value.size(); ++i) {
            std::string item_path = path + "[" + std::to_string(i) + "]";
            if (const json* failed = find_mismatch(value[i], schema["items"], false, item_path)) {
                path = item_path;
                return failed;
            }
        }
    }
    if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            const json* item_schema = nullptr;
            if (schema.contains("properties") && schema["properties"].contains(key)) {
                item_schema = &schema["properties"][key];
            } else if (schema.contains("additionalProperties") &&
                       schema["additionalProperties"].is_object()) {
                item_schema = &schema["additionalProperties"];
            }
            if (!item_schema) continue;
            std::string item_path = path + "." + key;
            if (const json* failed = find_mismatch(item, *item_schema, false, item_path)) {
                path = item_path;
                return failed;
            }
        }
    }
    return nullptr;
}

bool allows_null(const json& schema) {
    if (!schema.contains("type")) return false;
    const json& types = schema["type"];
    return types.is_array() ? std::find(types.begin(), types.end(), "null") != types.end()
                            : types == "null";
}

std::string describe_schema(const json& schema) {
    if (schema.contains("enum")) return "one of " + schema["enum"].dump();
    if (schema.contains("const")) return schema["const"].dump();
    std::string text = "a valid value";
    if (schema.contains("type")) {
        const json& types = schema["type"];
        if (types.is_string()) {
            text = types.get<std::string>();
        } else {
            text.clear();
            for (size_t i = 0; i < types.size(); ++i) {
                text += (i == 0 ? "" : i + 1 == types.size() ? " or " : ", ") +
                        types[i].get<std::string>();
            }
        }
    }
    if (schema.contains("minimum") && schema.contains("maximum")) {
        text += " from " + schema["minimum"].dump() + " to " + schema["maximum"].dump();
    } else if (schema.contains("minimum")) {
        text += " of at least " + schema["minimum"].dump();
    } else if (schema.contains("maximum")) {
        text += " of at most " + schema["maximum"].dump();
    }
    if (schema.contains("minLength")) {
        text = schema["minLength"] == 1 ? "non-empty " + text
                                        : text + " of at least " + schema["minLength"].dump() +
                                              " characters";
    }
    if (schema.contains("maxLength")) {
        text += " of at most " + schema["maxLength"].dump() + " characters";
    }
    if (schema.contains("pattern")) text += " matching " + schema["pattern"].dump();
    return text;
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

// Query and path values arrive as strings. A value that spells the schema's integer, number
// or boolean becomes that type; any other stays a string for the route to reject.
json typed_param(const std::string& text, const json& schema) {
    if (!schema.contains("type")) return text;
    const json& types = schema["type"];
    for (const auto& type : types.is_array() ? types : json::array({types})) {
        const std::string name = type.get<std::string>();
        try {
            size_t used = 0;
            if (name == "integer" && matches_query_type(text, name)) {
                const long long number = std::stoll(text, &used);
                if (used == text.size()) return number;
            } else if (name == "number" && matches_query_type(text, name)) {
                return std::stod(text, &used);
            }
        } catch (const std::exception&) {
        }
        if (name == "boolean") return text == "true";
    }
    return text;
}

// httplib numbers a path's capture groups in the order its {name} parameters appear, and
// RouteRegistry gives every path of a route the same parameters in the same order.
void collect_params(RouteRequest& req) {
    if (!req.spec.paths.empty()) {
        const std::string& path = req.spec.paths.front();
        size_t group = 0;
        for (size_t open = path.find('{'); open != std::string::npos; open = path.find('{', open + 1)) {
            ++group;
            const size_t close = path.find('}', open);
            const std::string name = path.substr(open + 1, close - open - 1);
            for (const auto& arg : req.spec.args) {
                if (arg.in == ArgIn::Path && arg.name == name && group < req.http.matches.size()) {
                    req.params[arg.name] = typed_param(req.http.matches[group].str(), arg.schema);
                }
            }
        }
    }
    for (const auto& arg : req.spec.args) {
        if (arg.in != ArgIn::Query) continue;
        std::vector<std::string> names = {arg.name};
        names.insert(names.end(), arg.aliases.begin(), arg.aliases.end());
        for (const auto& name : names) {
            if (req.http.has_param(name)) {
                req.params[arg.name] = typed_param(req.http.get_param_value(name), arg.schema);
                break;
            }
        }
    }
}

const json kAbsent;

const json& argument_source(const RouteRequest& req, const RouteArg& arg) {
    return arg.in == ArgIn::Query || arg.in == ArgIn::Path ? req.params : req.body;
}

// The streaming plumbing reports a backend failure as a payload in the sink rather than
// throwing: an SSE-style "data: {\"error\":...}" event or the backend's own JSON error body.
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
            return kModelUnloaded.status;
        }
    }

    return default_status_code;
}

void set_router_residency_conflict_response(const std::exception& error, httplib::Response& res) {
    res.status = ModelLoader::kRouterResidencyConflict.status;
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

nlohmann::ordered_json route_spec_to_json(const RouteSpec& spec) {
    using ordered_json = nlohmann::ordered_json;
    ordered_json args = ordered_json::array();
    for (const auto& arg : spec.args) {
        args.push_back({
            {"name", arg.name},
            {"in", to_string(arg.in)},
            {"schema", arg.schema},
            {"required", arg.required},
            {"supported", to_string(arg.supported)},
            {"description", arg.description},
            {"aliases", arg.aliases},
            {"forwarded", arg.forwarded},
        });
    }
    ordered_json responses = ordered_json::array();
    for (const auto& response : spec.responses) {
        responses.push_back({{"format", to_string(response.format)}, {"schema", response.schema}});
    }
    ordered_json examples = ordered_json::array();
    for (const auto& example : spec.examples) {
        ordered_json setup = ordered_json::array();
        for (const auto& step : example.setup) {
            setup.push_back({{"route", step.route}, {"example", step.example}, {"args", step.args}});
        }
        examples.push_back({
            {"name", example.name},
            {"description", example.description},
            {"format", to_string(example.format)},
            {"args", example.args},
            {"setup", setup},
        });
    }
    ordered_json errors = ordered_json::array();
    for (const auto& error : spec.errors) {
        errors.push_back({{"status", error.status}, {"code", error.code}, {"when", error.when}});
    }
    return {
        {"id", spec.id},
        {"methods", spec.methods},
        {"paths", spec.paths},
        {"prefixes", to_string(spec.prefixes)},
        {"summary", spec.summary},
        {"modality", spec.modality},
        {"description", spec.description},
        {"notes", spec.notes},
        {"experimental", spec.experimental},
        {"args", args},
        {"responses", responses},
        {"examples", examples},
        {"errors", errors},
        {"error_format", to_string(spec.error_format)},
        {"request_format", to_string(spec.request_format)},
        {"quiet_log", spec.quiet_log},
    };
}

bool RouteRequest::has(const RouteArg& arg) const {
    const json& source = argument_source(*this, arg);
    return source.is_object() && source.contains(arg.name);
}

const json& RouteRequest::arg(const RouteArg& arg) const {
    const json& source = argument_source(*this, arg);
    if (!source.is_object()) return kAbsent;
    auto it = source.find(arg.name);
    return it == source.end() ? kAbsent : *it;
}

const httplib::FormData* RouteRequest::file(const RouteArg& arg) const {
    for (const auto& [field, file] : http.form.files) {
        if (field == arg.name ||
            std::find(arg.aliases.begin(), arg.aliases.end(), field) != arg.aliases.end()) {
            return &file;
        }
    }
    return nullptr;
}

void ApiRoute::serve(const RouteSpec& spec, const httplib::Request& http,
                     httplib::Response& res) {
    RouteRequest req{http, spec};
    collect_params(req);

    if (spec.request_format == RequestFormat::Json ||
        spec.request_format == RequestFormat::OptionalJson) {
        const bool optional = spec.request_format == RequestFormat::OptionalJson;
        if (http.body.empty()) {
            if (!optional) {
                write_error(res, spec.error_format, kBodyNotJson,
                            "Request body is required but was empty");
                return;
            }
            req.body = json::object();
        } else {
            try {
                req.body = json::parse(http.body);
            } catch (const json::exception& e) {
                // Not just parse_error: a numeric literal too large for a double raises
                // out_of_range, which is answered the same way.
                write_error(res, spec.error_format, kBodyNotJson,
                            std::string("Invalid JSON in request body: ") + e.what());
                return;
            }
        }
        // Released CLIs send the literal null to POST /unload when no model is named.
        if (optional && req.body.is_null()) {
            req.body = json::object();
        }
        if (!req.body.is_object()) {
            write_error(res, spec.error_format, kBodyNotJson, "Request body must be a JSON object");
            return;
        }
        if (!check_args(req, res)) {
            return;
        }
    } else if (spec.request_format == RequestFormat::Raw && !check_args(req, res, true)) {
        return;
    }

    handle(req, res);
}

namespace {

bool is_empty_string(const json& value) {
    return value.is_string() && value.get_ref<const std::string&>().empty();
}

// Ollama's Go client sends both model and name and leaves the one it does not use empty.
void drop_empty_spellings(json& body, const std::vector<std::string>& names) {
    const bool any_filled =
        std::any_of(names.begin(), names.end(), [&body](const std::string& name) {
            auto it = body.find(name);
            return it != body.end() && !it->is_null() && !is_empty_string(*it);
        });
    if (!any_filled) return;
    for (const auto& name : names) {
        auto it = body.find(name);
        if (it != body.end() && is_empty_string(*it)) body.erase(it);
    }
}

} // namespace

bool ApiRoute::check_args(RouteRequest& req, httplib::Response& res, bool query_only) const {
    for (const auto& arg : req.spec.args) {
        if (arg.forwarded || arg.in == ArgIn::Path || (query_only && arg.in != ArgIn::Query)) {
            continue;
        }
        std::vector<std::string> names = {arg.name};
        names.insert(names.end(), arg.aliases.begin(), arg.aliases.end());
        if (arg.in == ArgIn::JsonBody && names.size() > 1) {
            drop_empty_spellings(req.body, names);
        }
        bool present = false;
        for (const auto& name : names) {
            json value;
            if (arg.in == ArgIn::JsonBody) {
                auto it = req.body.find(name);
                if (it == req.body.end()) continue;
                if (it->is_null()) {
                    if (!allows_null(arg.schema)) {
                        req.body.erase(it);
                    }
                    continue;
                }
                value = *it;
            } else if (arg.in == ArgIn::Query) {
                if (!req.http.has_param(name)) continue;
                value = req.http.get_param_value(name);
            } else if (is_file_arg(arg)) {
                if (req.http.form.has_file(name)) present = true;
                continue;
            } else {
                auto it = req.body.find(name);
                if (it == req.body.end()) continue;
                value = *it;
            }
            present = true;
            std::string path = name;
            if (const json* failed =
                    find_mismatch(value, arg.schema, arg.in == ArgIn::Query, path)) {
                write_error(res, req.spec.error_format, kArgInvalid,
                            "Invalid value for '" + path + "': expected " +
                                describe_schema(*failed));
                return false;
            }
        }
        if (arg.in == ArgIn::JsonBody) {
            for (const auto& alias : arg.aliases) {
                auto it = req.body.find(alias);
                if (it == req.body.end()) continue;
                if (!req.body.contains(arg.name)) {
                    req.body[arg.name] = std::move(*it);
                }
                req.body.erase(alias);
            }
        }
        if (!present && arg.required && !query_only) {
            std::string message = "Missing required argument '" + arg.name + "'";
            for (const auto& alias : arg.aliases) {
                message += " (or '" + alias + "')";
            }
            write_error(res, req.spec.error_format, kArgMissing, message);
            return false;
        }
    }
    return true;
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

        const RouteArg& model = model_arg();
        if (req.arg(model).is_string()) {
            req.model = canonical_model_name(ctx_, req.arg(model).get<std::string>());
            req.body[model.name] = req.model;
        }

        if (!validate(req, res)) {
            return;
        }

        if (!req.has(model)) {
            write_openai_error(res, kModelMissing, "Missing '" + model.name + "' field in request");
            return;
        }

        if (!req.model_already_loaded) {
            const LoadSpan load_span_name = load_span();
            auto span = load_span_name.kind.empty()
                ? nullptr
                : telemetry::TelemetryTracker::start_span(load_span_name.kind,
                                                          load_span_name.name, req.model,
                                                          req.body);
            try {
                ctx_.model_loader->ensure_loaded(req.model,
                                                 ModelLoader::load_options(req.arg(model_loading::kCtxSize)));
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
    write_plain_error(res, kUnexpectedException, error.what());
}

bool ModelRoute::build_form_body(RouteRequest& req, httplib::Response& res) {
    if (!req.http.is_multipart_form_data()) {
        write_openai_error(res, kFormNotMultipart, "Request must be multipart/form-data");
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
                const double parsed = std::stod(value, &pos);
                if (pos != value.size()) throw std::invalid_argument("trailing characters");
                if (std::isnan(parsed) || std::isinf(parsed)) {
                    throw std::invalid_argument("nan/inf not allowed");
                }
                req.body[arg.name] = parsed;
            } else {
                req.body[arg.name] = value;
            }
        } catch (const std::exception&) {
            write_error(res, req.spec.error_format, kArgInvalid,
                        "Invalid value for '" + arg.name + "': expected " + type);
            return false;
        }
    }
    return check_args(req, res);
}

std::string strip_latest_tag(const std::string& name) {
    const std::string suffix = ":latest";
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return name.substr(0, name.size() - suffix.size());
    }
    return name;
}

std::string canonical_model_name(ServerContext& ctx, const std::string& name, bool strip_latest) {
    std::string base = strip_latest ? strip_latest_tag(name) : name;
    if (base != name &&
        (ctx.alias_manager->has_alias(name) || ctx.model_manager->model_exists(name))) {
        base = name;
    }
    if (auto target = ctx.alias_manager->resolve_alias(base)) {
        return *target;
    }
    return base;
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
    LOG(INFO, "Telemetry") << "Inference completed: model=" << req.model
                           << ", tokens=" << (telemetry.input_tokens + telemetry.output_tokens)
                           << " (in=" << telemetry.input_tokens
                           << ", out=" << telemetry.output_tokens << ")"
                           << ", ttft=" << std::fixed << std::setprecision(2)
                           << telemetry.time_to_first_token << "s"
                           << ", tps=" << telemetry.tokens_per_second << std::endl;

    ctx_.router->update_request_telemetry(req.model, telemetry);
}

void set_error_response(const json& response, httplib::Response& res, const RouteError& fallback) {
    res.status = get_error_status_code(response, fallback.status);
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
        res.status = kNoOutputError.status;
        res.set_content(json{{"error", {
            {"message", "Generation failed: the backend produced no output (it likely crashed or ran "
                        "out of GPU memory). Check the server logs."},
            {"type", "backend_error"}}}}.dump(), "application/json");
        return;
    }
    if (auto error_payload = extract_error_payload(buf); !error_payload.is_null()) {
        res.status = get_error_status_code(error_payload, kBackendError.status);
        res.set_content(error_payload.dump(), "application/json");
        return;
    }
    res.set_content(buf, mime_type);
}

bool write_sse_event(httplib::DataSink& sink, const std::string& event, const json& data) {
    std::string payload = "event: " + event + "\ndata: " + data.dump() + "\n\n";
    return sink.write(payload.c_str(), payload.size());
}

std::vector<RouteError> join_errors(std::initializer_list<std::vector<RouteError>> lists) {
    std::vector<RouteError> errors;
    for (const auto& list : lists) {
        errors.insert(errors.end(), list.begin(), list.end());
    }
    return errors;
}

void write_openai_error(httplib::Response& res, const RouteError& route_error,
                        const std::string& message, const std::string& type) {
    json error = {{"message", message}, {"type", type}};
    if (!route_error.code.empty()) {
        error["code"] = route_error.code;
    }
    res.status = route_error.status;
    res.set_content(json{{"error", error}}.dump(), "application/json");
}

void write_plain_error(httplib::Response& res, const RouteError& error, const std::string& message) {
    json body = {{"error", message}};
    if (!error.code.empty()) {
        body["code"] = error.code;
    }
    res.status = error.status;
    res.set_content(body.dump(), "application/json");
}

void write_anthropic_error(httplib::Response& res, const RouteError& error,
                           const std::string& message) {
    json body = anthropic::build_anthropic_error(json{{"message", message}}, error.status);
    if (!error.code.empty()) {
        body["error"]["code"] = error.code;
    }
    res.status = error.status;
    res.set_content(body.dump(), "application/json");
}

void write_error(httplib::Response& res, ErrorFormat format, const RouteError& error,
                 const std::string& message) {
    switch (format) {
        case ErrorFormat::Plain: write_plain_error(res, error, message); return;
        case ErrorFormat::OpenAi: write_openai_error(res, error, message); return;
        case ErrorFormat::Anthropic: write_anthropic_error(res, error, message); return;
    }
}

} // namespace lemon
