#include "lemon/ollama_api.h"
#include <iostream>
#include <lemon/utils/aixlog.hpp>

namespace lemon {

namespace {

json anthropic_error(const std::string& type, const std::string& message) {
    return {
        {"type", "error"},
        {"error", {{"type", type}, {"message", message}}},
    };
}

int infer_status_from_error(const json& response) {
    if (!response.contains("error") || !response["error"].is_object()) {
        return 200;
    }

    const auto& error = response["error"];

    if (error.contains("details") && error["details"].is_object()) {
        const auto& details = error["details"];
        if (details.contains("status_code") && details["status_code"].is_number_integer()) {
            return details["status_code"].get<int>();
        }
    }

    if (error.contains("status_code") && error["status_code"].is_number_integer()) {
        return error["status_code"].get<int>();
    }

    const std::string type = error.value("type", "");
    if (type == "invalid_request" || type == "invalid_request_error") {
        return 400;
    }
    if (type == "model_not_loaded" || type == "not_found_error") {
        return 404;
    }
    if (type == "unsupported_operation") {
        return 400;
    }

    return 500;
}

bool validate_messages_request(const json& request_json, httplib::Response& res) {
    const std::string model = request_json.value("model", "");
    if (model.empty()) {
        res.status = 400;
        res.set_content(
            anthropic_error("invalid_request_error", "model is required").dump(),
            "application/json");
        return false;
    }

    if (!request_json.contains("messages") || !request_json["messages"].is_array()) {
        res.status = 400;
        res.set_content(
            anthropic_error("invalid_request_error", "messages must be an array").dump(),
            "application/json");
        return false;
    }

    return true;
}

bool apply_raw_backend_passthrough_if_present(const json& response, httplib::Response& res) {
    if (!response.contains("_lemonade_raw_backend") || !response["_lemonade_raw_backend"].is_object()) {
        return false;
    }

    const auto& raw = response["_lemonade_raw_backend"];
    const int status = raw.value("status_code", 500);
    const std::string content_type = raw.value("content_type", "application/json");
    const std::string body = raw.value("body", "{}");

    res.status = status;
    res.set_content(body, content_type.c_str());
    return true;
}

void update_telemetry_from_anthropic_usage_if_present(Router* router, const json& response) {
    if (!response.contains("usage") || !response["usage"].is_object()) {
        return;
    }

    const auto& usage = response["usage"];
    const int input_tokens = usage.value("input_tokens", 0);
    const int output_tokens = usage.value("output_tokens", 0);

    LOG(INFO, "Telemetry") << "=== Telemetry ===" << std::endl;
    LOG(INFO, "Telemetry") << "Input tokens:  " << input_tokens << std::endl;
    LOG(INFO, "Telemetry") << "Output tokens: " << output_tokens << std::endl;
    LOG(INFO, "Telemetry") << "=================" << std::endl;

    router->update_telemetry(input_tokens, output_tokens, 0.0, 0.0);
    router->update_prompt_tokens(input_tokens);
}

}  // namespace

void OllamaApi::register_anthropic_routes(httplib::Server& server, const std::shared_ptr<OllamaApi>& self) {
    server.Post("/v1/messages", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_anthropic_messages(req, res);
    });

    server.Post("/v1/messages/count_tokens", [self](const httplib::Request& req, httplib::Response& res) {
        self->handle_anthropic_count_tokens(req, res);
    });
}

void OllamaApi::handle_anthropic_messages(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);
        if (!validate_messages_request(request_json, res)) {
            return;
        }

        std::string model = normalize_model_name(request_json.value("model", ""));
        request_json["model"] = model;

        try {
            auto_load_model(model);
        } catch (const std::exception&) {
            res.status = 404;
            res.set_content(
                anthropic_error("not_found_error", "model '" + model + "' not found, try pulling it first").dump(),
                "application/json");
            return;
        }

        const bool stream = request_json.value("stream", false);
        if (stream) {
            const std::string body = request_json.dump();
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");

            res.set_chunked_content_provider(
                "text/event-stream",
                [this, body](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) {
                        return false;
                    }

                    router_->anthropic_messages_stream(body, sink);
                    return false;
                });
            return;
        }

        auto response = router_->anthropic_messages(request_json);
        if (apply_raw_backend_passthrough_if_present(response, res)) {
            return;
        }

        update_telemetry_from_anthropic_usage_if_present(router_, response);

        const int status = infer_status_from_error(response);
        if (status != 200) {
            res.status = status;
        }
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /v1/messages: " << e.what() << std::endl;
        res.status = 500;
        res.set_content(anthropic_error("api_error", e.what()).dump(), "application/json");
    }
}

void OllamaApi::handle_anthropic_count_tokens(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = json::parse(req.body);
        if (!validate_messages_request(request_json, res)) {
            return;
        }

        std::string model = normalize_model_name(request_json.value("model", ""));
        request_json["model"] = model;

        try {
            auto_load_model(model);
        } catch (const std::exception&) {
            res.status = 404;
            res.set_content(
                anthropic_error("not_found_error", "model '" + model + "' not found, try pulling it first").dump(),
                "application/json");
            return;
        }

        auto response = router_->anthropic_count_tokens(request_json);
        if (apply_raw_backend_passthrough_if_present(response, res)) {
            return;
        }

        const int status = infer_status_from_error(response);
        if (status != 200) {
            res.status = status;
        }
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[OllamaApi] Error in /v1/messages/count_tokens: " << e.what() << std::endl;
        res.status = 500;
        res.set_content(anthropic_error("api_error", e.what()).dump(), "application/json");
    }
}

}  // namespace lemon
