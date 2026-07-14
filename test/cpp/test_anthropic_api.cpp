#include "lemon/ollama_api.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <vector>

namespace lemon {

class AnthropicApiTestPeer {
public:
    static json convert_request(OllamaApi& api,
                                const json& request,
                                std::vector<std::string>* returned_warnings = nullptr) {
        std::vector<std::string> warnings;
        auto result = api.convert_anthropic_to_openai_chat(request, warnings);
        if (returned_warnings) {
            *returned_warnings = warnings;
        }
        return result;
    }

    static json convert_response(OllamaApi& api, const json& response) {
        std::vector<std::string> warnings;
        return api.convert_openai_chat_to_anthropic(response, "test-model", warnings);
    }

    static httplib::Response handle_messages(OllamaApi& api, const std::string& body) {
        httplib::Request request;
        request.body = body;
        httplib::Response response;
        api.handle_anthropic_messages(request, response);
        return response;
    }

    static std::string stream(OllamaApi& api,
                              int input_tokens,
                              const std::vector<std::string>& chunks) {
        std::string output;
        httplib::DataSink sink;
        sink.is_writable = [] { return true; };
        sink.write = [&output](const char* data, size_t size) {
            output.append(data, size);
            return true;
        };
        sink.done = [] {};

        api.stream_openai_sse_to_anthropic_sse(
            "{}", sink, "test-model", input_tokens,
            [&chunks](const std::string&, httplib::DataSink& backend_sink) {
                for (const auto& chunk : chunks) {
                    assert(backend_sink.write(chunk.data(), chunk.size()));
                }
                backend_sink.done();
            });
        return output;
    }
};

static json base_request() {
    return {
        {"model", "test-model"},
        {"max_tokens", 32},
        {"messages", json::array({
            {{"role", "user"}, {"content", "hello"}}
        })}
    };
}

static void test_validation() {
    OllamaApi api(nullptr, nullptr);

    auto missing_messages = AnthropicApiTestPeer::handle_messages(
        api, R"({"model":"test-model","max_tokens":8})");
    assert(missing_messages.status == 400);
    auto error = json::parse(missing_messages.body);
    assert(error["error"]["type"] == "invalid_request_error");

    auto invalid_role = AnthropicApiTestPeer::handle_messages(
        api,
        R"({"model":"test-model","max_tokens":8,"messages":[{"role":"system","content":"x"}]})");
    assert(invalid_role.status == 400);

    auto invalid_thinking_signature = AnthropicApiTestPeer::handle_messages(
        api,
        R"({"model":"test-model","max_tokens":8,"messages":[{"role":"assistant","content":[{"type":"thinking","thinking":"x","signature":1}]}]})");
    assert(invalid_thinking_signature.status == 400);

    auto invalid_redacted_thinking = AnthropicApiTestPeer::handle_messages(
        api,
        R"({"model":"test-model","max_tokens":8,"messages":[{"role":"assistant","content":[{"type":"redacted_thinking","data":1}]}]})");
    assert(invalid_redacted_thinking.status == 400);

    auto invalid_sampling = AnthropicApiTestPeer::handle_messages(
        api,
        R"({"model":"test-model","max_tokens":8,"temperature":2,"messages":[{"role":"user","content":"x"}]})");
    assert(invalid_sampling.status == 400);

    auto invalid_thinking_tools = AnthropicApiTestPeer::handle_messages(
        api,
        R"({"model":"test-model","max_tokens":8,"thinking":{"type":"adaptive"},"tools":[{"name":"x","input_schema":{}}],"tool_choice":{"type":"any"},"messages":[{"role":"user","content":"x"}]})");
    assert(invalid_thinking_tools.status == 400);
}

static void test_request_conversion() {
    OllamaApi api(nullptr, nullptr);
    auto request = base_request();
    request["system"] = json::array({
        {{"type", "text"}, {"text", "alpha"}},
        {{"type", "text"}, {"text", "beta"}}
    });
    request["messages"] = json::array({
        {
            {"role", "assistant"},
            {"content", json::array({
                {{"type", "thinking"}, {"thinking", "private"}, {"signature", "remote"}},
                {{"type", "redacted_thinking"}, {"data", "opaque"}},
                {{"type", "tool_use"}, {"id", "call_1"}, {"name", "inspect"}, {"input", json::object()}}
            })}
        },
        {
            {"role", "user"},
            {"content", json::array({
                {
                    {"type", "tool_result"},
                    {"tool_use_id", "call_1"},
                    {"content", json::array({
                        {{"type", "text"}, {"text", "image:"}},
                        {{"type", "image"}, {"source", {
                            {"type", "url"}, {"url", "https://example.com/image.png"}
                        }}}
                    })}
                },
                {{"type", "text"}, {"text", "continue"}}
            })}
        }
    });
    request["tools"] = json::array({
        {{"name", "ignore"}, {"input_schema", json::object()}},
        {{"name", "inspect"}, {"input_schema", json::object()}}
    });
    request["tool_choice"] = {
        {"type", "tool"},
        {"name", "inspect"},
        {"disable_parallel_tool_use", false}
    };

    std::vector<std::string> warnings;
    auto converted = AnthropicApiTestPeer::convert_request(api, request, &warnings);
    assert(converted["messages"][0]["content"] == "alphabeta");
    assert(converted["messages"][1]["reasoning_content"] == "private");
    assert(converted["messages"][2]["content"].is_array());
    assert(converted["messages"][2]["content"][1]["type"] == "image_url");
    assert(converted["messages"][3]["content"] == "continue");
    assert(converted["tools"].size() == 1);
    assert(converted["tools"][0]["function"]["name"] == "inspect");
    assert(converted["tool_choice"] == "required");
    assert(converted["parallel_tool_calls"] == true);
    assert(warnings == std::vector<std::string>({
        "Ignored unverifiable signature on thinking block",
        "Ignored redacted_thinking block"
    }));

    auto adaptive_request = base_request();
    adaptive_request["thinking"] = {{"type", "adaptive"}};
    auto adaptive = AnthropicApiTestPeer::convert_request(api, adaptive_request);
    assert(adaptive["enable_thinking"] == true);

    for (const auto& choice : {
             json{{"type", "auto"}},
             json{{"type", "any"}},
             json{{"type", "none"}}}) {
        auto mode_request = base_request();
        mode_request["tools"] = request["tools"];
        mode_request["tool_choice"] = choice;
        auto mode = AnthropicApiTestPeer::convert_request(api, mode_request);
        const std::string expected = choice["type"] == "any"
            ? "required"
            : choice["type"].get<std::string>();
        assert(mode["tool_choice"] == expected);
    }
}

static void test_response_conversion() {
    OllamaApi api(nullptr, nullptr);
    json response = {
        {"choices", json::array({{
            {"finish_reason", "stop"},
            {"stopping_word", "END"},
            {"message", {
                {"content", "done"},
                {"tool_calls", json::array({{
                    {"type", "function"},
                    {"function", {{"name", "inspect"}, {"arguments", "{}"}}}
                }})}
            }}
        }})},
        {"usage", {
            {"prompt_tokens", 10},
            {"completion_tokens", 3},
            {"prompt_tokens_details", {{"cached_tokens", 4}}}
        }}
    };

    auto first = AnthropicApiTestPeer::convert_response(api, response);
    auto second = AnthropicApiTestPeer::convert_response(api, response);
    assert(first["stop_reason"] == "tool_use");
    assert(first["stop_sequence"] == "END");
    assert(first["usage"]["input_tokens"] == 6);
    assert(first["usage"]["cache_read_input_tokens"] == 4);
    assert(!first.contains("warnings"));
    assert(first["content"][1]["id"].get<std::string>().rfind("toolu_", 0) == 0);
    assert(first["content"][1]["id"] != second["content"][1]["id"]);
}

static void test_stream_conversion() {
    OllamaApi api(nullptr, nullptr);
    std::vector<std::string> chunks = {
        "data: {\"id\":\"backend-id\",\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"x\\\":\"}}]},\"finish_reason\":null}]}\n\n",
        "data:{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"name\":\"inspect\",\"arguments\":\"1}\"}}]},\"finish_reason\":\"tool_calls\"}]}\n\n",
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":7,\"completion_tokens\":3}}\n\n",
        "data: [DONE]\n\n"
    };
    const std::string output = AnthropicApiTestPeer::stream(api, 7, chunks);
    assert(output.find("\"input_tokens\":7") != std::string::npos);
    assert(output.find("\"id\":\"call_1\"") != std::string::npos);
    assert(output.find("\"name\":\"inspect\"") != std::string::npos);
    assert(output.find("unknown_tool") == std::string::npos);
    assert(output.find("\"partial_json\":\"{\\\"x\\\":1}\"") != std::string::npos);
    assert(output.find("\"index\":0") != std::string::npos);
    assert(output.find("\"usage\":{\"output_tokens\":3}") != std::string::npos);
    assert(output.find("event: message_stop") != std::string::npos);

    const std::string malformed = AnthropicApiTestPeer::stream(
        api, 1, {"data: {not-json}\n\n"});
    assert(malformed.find("event: error") != std::string::npos);
    assert(malformed.find("event: message_stop") == std::string::npos);

    const std::string permission_error = AnthropicApiTestPeer::stream(
        api, 1, {"data: {\"error\":{\"message\":\"denied\",\"status_code\":403}}\n\n"});
    assert(permission_error.find("permission_error") != std::string::npos);

    const std::string overloaded_error = AnthropicApiTestPeer::stream(
        api, 1, {"data: {\"error\":{\"message\":\"busy\",\"status_code\":529}}\n\n"});
    assert(overloaded_error.find("overloaded_error") != std::string::npos);
}

} // namespace lemon

int main() {
    lemon::test_validation();
    lemon::test_request_conversion();
    lemon::test_response_conversion();
    lemon::test_stream_conversion();
    return 0;
}
