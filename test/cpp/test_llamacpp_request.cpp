#include "lemon/backends/llamacpp/llamacpp_request.h"

#include <cstdlib>
#include <iostream>
#include <string>

using lemon::backends::llamacpp::sanitize_tool_schema_limits;
using nlohmann::json;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "ok: " << message << std::endl;
        return;
    }
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
}

}  // namespace

int main() {
    const json chat_request = {
        {"model", "test"},
        {"tools", {{
            {"type", "function"},
            {"function", {
                {"name", "Workflow"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"script", {
                            {"type", "string"},
                            {"maxLength", 524288},
                        }},
                        {"name", {
                            {"type", "string"},
                            {"maxLength", 1999},
                        }},
                        {"steps", {
                            {"type", "array"},
                            {"maxItems", 2000},
                            {"items", {
                                {"$ref", "#/$defs/step"},
                            }},
                        }},
                    }},
                    {"$defs", {
                        {"step", {
                            {"type", "object"},
                            {"properties", {
                                {"output", {
                                    {"type", "string"},
                                    {"maxLength", 4096},
                                }},
                            }},
                        }},
                    }},
                }},
            }},
        }}},
    };

    const json sanitized_chat = sanitize_tool_schema_limits(chat_request);
    const json& parameters = sanitized_chat["tools"][0]["function"]["parameters"];
    expect(!parameters["properties"]["script"].contains("maxLength"),
           "removes oversized Chat Completions maxLength");
    expect(parameters["properties"]["name"]["maxLength"] == 1999,
           "preserves maxLength below the llama.cpp limit");
    expect(!parameters["properties"]["steps"].contains("maxItems"),
           "removes oversized maxItems");
    expect(!parameters["$defs"]["step"]["properties"]["output"].contains("maxLength"),
           "recurses through $defs");
    expect(chat_request["tools"][0]["function"]["parameters"]["properties"]["script"]["maxLength"] == 524288,
           "does not mutate the caller's request");

    const json responses_request = {
        {"tools", {{
            {"type", "function"},
            {"name", "Workflow"},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"script", {
                        {"type", "string"},
                        {"maxLength", 524288},
                    }},
                }},
            }},
        }}},
    };

    const json sanitized_responses = sanitize_tool_schema_limits(responses_request);
    expect(!sanitized_responses["tools"][0]["parameters"]["properties"]["script"].contains("maxLength"),
           "removes oversized Responses API maxLength");

    const json unrelated = {
        {"metadata", {
            {"maxLength", 524288},
        }},
    };
    expect(sanitize_tool_schema_limits(unrelated) == unrelated,
           "does not rewrite values outside tool schemas");

    if (failures == 0) {
        std::cout << "All llama.cpp request assertions passed" << std::endl;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
