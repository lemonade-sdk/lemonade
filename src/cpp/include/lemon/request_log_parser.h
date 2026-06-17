#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace lemon {

struct ParsedRequestBody {
    std::string model;
    std::string keep_alive;
    std::optional<bool> stream;
    int prompt_chars = 0;
    int messages_chars = 0;
    nlohmann::json redacted_body;
    bool has_redacted_body = false;
};

std::string classify_endpoint_type(const std::string& path, const std::string& method);

std::string extract_forwarded_for(const std::string& x_forwarded_for,
                                  const std::string& x_real_ip,
                                  const std::string& forwarded);

ParsedRequestBody parse_request_body(const std::string& body,
                                     const std::string& path,
                                     bool log_prompts);

nlohmann::json redact_json(const nlohmann::json& value);

std::string extract_response_error(const std::string& response_body, int status_code);

bool should_skip_request_log_path(const std::string& path, const std::string& method);

} // namespace lemon
