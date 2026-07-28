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
                            {"minLength", 1999},
                        }},
                        {"description", {
                            {"type", "string"},
                            {"minLength", 2000},
                        }},
                        {"floatDescription", {
                            {"type", "string"},
                            {"maxLength", 2000.0},
                        }},
                        {"fractionalDescription", {
                            {"type", "string"},
                            {"maxLength", 2000.5},
                        }},
                        {"negativeDescription", {
                            {"type", "string"},
                            {"maxLength", -2000.0},
                        }},
                        {"steps", {
                            {"type", "array"},
                            {"maxItems", 2000},
                            {"minItems", 1999},
                            {"items", {
                                {"$ref", "#/$defs/step"},
                            }},
                        }},
                        {"queue", {
                            {"type", "array"},
                            {"minItems", 2000},
                        }},
                        {"backlog", {
                            {"type", "array"},
                            {"minItems", 2001},
                        }},
                        {"floatBacklog", {
                            {"type", "array"},
                            {"minItems", 2001.0},
                        }},
                        {"results", {
                            {"type", "array"},
                            {"maxItems", 2001},
                        }},
                        {"config", {
                            {"const", {
                                {"maxLength", 524288},
                                {"minItems", 2000},
                            }},
                        }},
                        {"mode", {
                            {"enum", {
                                {
                                    {"maxItems", 2000},
                                },
                                {
                                    {"minLength", 4096},
                                },
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
    expect(parameters["properties"]["name"]["minLength"] == 1999,
           "preserves minLength below the llama.cpp limit");
    expect(!parameters["properties"]["description"].contains("minLength"),
           "removes minLength at the llama.cpp limit");
    expect(!parameters["properties"]["floatDescription"].contains("maxLength"),
           "removes integral floating-point maxLength at the llama.cpp limit");
    expect(parameters["properties"]["fractionalDescription"]["maxLength"] == 2000.5,
           "preserves non-integral floating-point bounds");
    expect(parameters["properties"]["negativeDescription"]["maxLength"] == -2000.0,
           "preserves negative floating-point bounds");
    expect(parameters["properties"]["steps"]["maxItems"] == 2000,
           "preserves maxItems whose generated repetition stays below the limit");
    expect(parameters["properties"]["steps"]["minItems"] == 1999,
           "preserves minItems below the llama.cpp limit");
    expect(parameters["properties"]["queue"]["minItems"] == 2000,
           "preserves minItems whose generated repetition stays below the limit");
    expect(!parameters["properties"]["backlog"].contains("minItems"),
           "removes minItems whose generated repetition reaches the limit");
    expect(!parameters["properties"]["floatBacklog"].contains("minItems"),
           "removes integral floating-point minItems whose repetition reaches the limit");
    expect(!parameters["properties"]["results"].contains("maxItems"),
           "removes maxItems whose generated repetition reaches the limit");
    expect(!parameters["$defs"]["step"]["properties"]["output"].contains("maxLength"),
           "recurses through $defs");
    expect(parameters["properties"]["config"]["const"] == chat_request["tools"][0]["function"]["parameters"]["properties"]["config"]["const"],
           "preserves object values inside const");
    expect(parameters["properties"]["mode"]["enum"] == chat_request["tools"][0]["function"]["parameters"]["properties"]["mode"]["enum"],
           "preserves object values inside enum");
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
