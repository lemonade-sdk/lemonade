#include "lemon/server/request_middleware.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <vector>

#include <lemon/utils/aixlog.hpp>
#include <nlohmann/json.hpp>

#include "../telemetry.h"
#include "lemon/runtime_config.h"
#include "lemon/server/route_registry.h"
#include "lemon/utils/origin_utils.h"
#include "lemon/utils/session_utils.h"

namespace lemon {

namespace {

// Parse a W3C Trace Context "traceparent" header per the spec grammar
// (https://www.w3.org/TR/trace-context/): version "-" trace-id "-" parent-id
// "-" trace-flags, where version is 2 lowercase hex chars, trace-id is 32,
// parent-id is 16, and trace-flags is 2. The all-zero trace-id and parent-id
// are invalid. On success, out_trace_id/out_parent_id receive the lowercase
// hex ids and the function returns true; otherwise returns false.
bool parse_traceparent(const std::string& header, std::string& out_trace_id, std::string& out_parent_id) {
    // A version-00 traceparent is exactly 55 chars. Reject anything shorter and
    // cap the length so a header packed with dashes can't drive an unbounded
    // split loop; the upper bound leaves room for higher versions that may
    // append trailing fields (handled below).
    if (header.size() < 55 || header.size() > 128) return false;

    auto is_hex = [](const std::string& s) {
        return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    auto is_all_zero = [](const std::string& s) {
        return s.find_first_not_of('0') == std::string::npos;
    };

    // Lowercase a copy so uppercase hex from lenient clients still validates.
    std::string h = header;
    std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return std::tolower(c); });

    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t dash = h.find('-', start);
        if (dash == std::string::npos) {
            parts.push_back(h.substr(start));
            break;
        }
        parts.push_back(h.substr(start, dash - start));
        start = dash + 1;
    }

    if (parts.size() < 4) return false;
    const std::string& version = parts[0];
    const std::string& trace_id = parts[1];
    const std::string& parent_id = parts[2];
    const std::string& flags = parts[3];

    if (version.size() != 2 || !is_hex(version) || version == "ff") return false;
    // Version 00 carries exactly four fields; per W3C forward-compatibility a
    // higher version may append trailing fields, which we parse then ignore.
    if (version == "00" && parts.size() != 4) return false;
    if (trace_id.size() != 32 || !is_hex(trace_id) || is_all_zero(trace_id)) return false;
    if (parent_id.size() != 16 || !is_hex(parent_id) || is_all_zero(parent_id)) return false;
    if (flags.size() != 2 || !is_hex(flags)) return false;

    out_trace_id = trace_id;
    out_parent_id = parent_id;
    return true;
}

} // namespace

RequestMiddleware::RequestMiddleware(RuntimeConfig* config, const RouteRegistry* registry)
    : config_(config), registry_(registry) {
    const char* api_key_env = std::getenv("LEMONADE_API_KEY");
    api_key_ = api_key_env ? std::string(api_key_env) : "";

    // Read admin API key - if not set, defaults to regular API key value
    const char* admin_api_key_env = std::getenv("LEMONADE_ADMIN_API_KEY");
    admin_api_key_ = admin_api_key_env ? std::string(admin_api_key_env) : api_key_;
}

void RequestMiddleware::install(httplib::Server& server) {
    server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        return pre_route(req, res);
    });

    server.set_default_headers({
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization, X-Client-Session-Id, X-Account-Session-Id, mcp-protocol-version, traceparent, Mcp-Session-Id"}
    });

    // CORS preflight
    server.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    server.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        LOG(ERROR, "Server") << "Error " << res.status << ": " << req.method << " " << req.path << std::endl;

        if (res.status == 404) {
            // Only set generic "endpoint not found" if no content was already set
            // This preserves specific error messages (e.g., "model not found")
            if (res.body.empty()) {
                nlohmann::json error = {
                    {"error", {
                        {"message", "The requested endpoint does not exist"},
                        {"type", "not_found"},
                        {"path", req.path}
                    }}
                };
                res.set_content(error.dump(), "application/json");
            }
        } else if (res.status == 400) {
            LOG(ERROR, "Server") << "400 Bad Request details - Body length: " << req.body.length()
                      << ", Content-Type: " << req.get_header_value("Content-Type") << std::endl;
            if (res.body.empty()) {
                nlohmann::json error = {
                    {"error", {
                        {"message", "Bad request"},
                        {"type", "bad_request"}
                    }}
                };
                res.set_content(error.dump(), "application/json");
            }
        }
    });

    server.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
        log_response(req, res);
    });
}

httplib::Server::HandlerResponse RequestMiddleware::pre_route(const httplib::Request& req,
                                                              httplib::Response& res) {
    if (!registry_->is_quiet(req.path)) {
        LOG(DEBUG, "Server") << req.method << " " << req.path << std::endl;
    }

    // Unconditionally set Vary: Origin to prevent caching issues, preserving existing values
    std::string vary = "Origin";
    if (res.has_header("Vary")) {
        std::string existing = res.get_header_value("Vary");
        if (existing.find("Origin") == std::string::npos) {
            vary = existing + ", Origin";
        } else {
            vary = existing;
        }
    }
    res.set_header("Vary", vary);

    if (req.has_header("Origin")) {
        std::string origin = req.get_header_value("Origin");
        std::string host = req.get_header_value("Host");
        std::string scheme = "http";

        std::string allowed_origins = utils::resolve_allowed_origins();
        std::string bound_host = config_ ? config_->host() : "";
        if (utils::is_origin_allowed(origin, allowed_origins, host, scheme, bound_host)) {
            res.set_header("Access-Control-Allow-Origin", origin);
            if (req.has_header("Access-Control-Request-Private-Network") &&
                req.get_header_value("Access-Control-Request-Private-Network") == "true") {
                res.set_header("Access-Control-Allow-Private-Network", "true");
            }
        } else {
            LOG(WARNING, "Server") << "Rejected request from unauthorized origin: " << origin
                                   << ". Configure allowed_origins in config.json or via 'lemonade config set allowed_origins=...' to allow this origin." << std::endl;
            res.status = 403;
            res.set_content("{\"error\": \"Origin not allowed\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
    }

    return authenticate(req, res);
}

httplib::Server::HandlerResponse RequestMiddleware::authenticate(const httplib::Request& req,
                                                                 httplib::Response& res) {
    telemetry::g_request_start_time = std::chrono::steady_clock::now();
    telemetry::g_current_auth_token = "";
    if (req.has_header("X-Client-Session-Id")) {
        telemetry::g_current_client_session_id = req.get_header_value("X-Client-Session-Id");
    } else {
        telemetry::g_current_client_session_id.clear();
    }

    // Opt-in W3C Trace Context ingestion: when enabled, adopt a valid incoming
    // "traceparent" so inference spans join the caller's distributed trace
    // instead of starting a fresh root. Cleared otherwise so a stale thread-local
    // never leaks across requests on a reused worker thread.
    telemetry::g_incoming_trace_id.clear();
    telemetry::g_incoming_parent_span_id.clear();
    if (config_->telemetry_trust_incoming_trace_context() && req.has_header("traceparent")) {
        std::string trace_id;
        std::string parent_id;
        if (parse_traceparent(req.get_header_value("traceparent"), trace_id, parent_id)) {
            telemetry::g_incoming_trace_id = trace_id;
            telemetry::g_incoming_parent_span_id = parent_id;
        }
    }

    telemetry::g_incoming_client_id.clear();
    telemetry::g_incoming_session_id.clear();

    session::SessionContext telemetry_session = config_
        ? session::resolve_session_context(
              req,
              config_->telemetry_session_headers_id(),
              config_->telemetry_session_headers_client())
        : session::resolve_session_context(req, {}, {});
    if (!telemetry_session.session_id.empty()) {
        telemetry::g_incoming_client_id = telemetry_session.client_id;
        telemetry::g_incoming_session_id = telemetry_session.session_id;
    }

    // Reset every request so a reused worker thread can't leak the prior
    // caller's session to a cloud provider.
    session::g_request_session = session::resolve_forwardable_session(req);

    // Authentication is decided by path, so a route cannot opt out of it. /mcp is the
    // one API route outside the /api/, /v0/ and /v1/ prefixes, so it is named here to
    // keep LEMONADE_API_KEY enforcement on the MCP gateway (Invariant #2).
    bool is_api_route = (req.path.rfind("/api/", 0) == 0) ||
                        (req.path.rfind("/v0/", 0) == 0) ||
                        (req.path.rfind("/v1/", 0) == 0) ||
                        (req.path == "/mcp");
    bool is_internal_route = (req.path.rfind("/internal/", 0) == 0);
    bool is_metrics_route = (req.path == "/metrics");

    // api_key_ gates the regular API endpoints (/api, /v0, /v1); admin_api_key_
    // gates /internal/* and defaults to api_key_ when LEMONADE_ADMIN_API_KEY is
    // unset. The admin key also authenticates the regular endpoints. An empty
    // key leaves its endpoints open.

    // Safely extract bearer token, guarding against malformed Authorization headers
    std::string auth_token;
    try {
        if (req.has_header("Authorization")) {
            auto auth_value = req.get_header_value("Authorization");
            // httplib::get_bearer_token_auth does substr(7) for "Bearer ", so check length
            if (auth_value.size() >= 7) {
                auth_token = httplib::get_bearer_token_auth(req);
            }
            // Silently ignore malformed/short Authorization headers
        }
    } catch (const std::exception& e) {
        LOG(DEBUG, "Server") << "Failed to parse Authorization header: " << e.what() << std::endl;
    }

    telemetry::g_current_auth_token = auth_token;

    if (is_internal_route) {
        if (!admin_api_key_.empty() && req.method != "OPTIONS") {
            if (auth_token != admin_api_key_) {
                res.status = 401;
                res.set_content("{\"error\": \"Invalid or missing admin API key\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
    } else if ((is_api_route || is_metrics_route) && req.method != "OPTIONS") {
        if (!api_key_.empty()) {
            if ((auth_token != api_key_) && (auth_token != admin_api_key_)) {
                res.status = 401;
                res.set_content("{\"error\": \"Invalid or missing API key\"}", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }
    }

    return httplib::Server::HandlerResponse::Unhandled;
}

void RequestMiddleware::log_response(const httplib::Request& req, const httplib::Response& res) {
    // A scraper polls /metrics constantly; log its first success and every failure.
    if (req.path == "/metrics") {
        if (res.status == 200) {
            bool expected = false;
            if (metrics_access_logged_.compare_exchange_strong(expected, true)) {
                LOG(INFO, "Server") << req.method << " " << req.path << " - " << res.status << std::endl;
            }
        } else {
            LOG(WARNING, "Server") << req.method << " " << req.path << " - " << res.status << std::endl;
        }
        return;
    }

    if (registry_->is_quiet(req.path)) {
        return;
    }

    // Static assets and the page shell are fetched on every UI load.
    bool is_quiet_get = (req.method == "GET" && (
        req.path == "/" ||
        req.path.find(".js") != std::string::npos ||
        req.path.find(".css") != std::string::npos ||
        req.path.find(".svg") != std::string::npos ||
        req.path.find(".png") != std::string::npos ||
        req.path.find(".ico") != std::string::npos ||
        req.path.find(".woff") != std::string::npos
    ));

    if (!is_quiet_get) {
        LOG(DEBUG, "Server") << req.method << " " << req.path << " - " << res.status << std::endl;
    }
}

} // namespace lemon
