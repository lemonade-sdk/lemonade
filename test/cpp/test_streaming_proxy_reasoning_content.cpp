#include "lemon/streaming_proxy.h"
#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

using json = nlohmann::json;

static json parse_first_data_json(const std::string& sse) {
    const std::string prefix = "data: ";
    auto start = sse.find(prefix);
    assert(start != std::string::npos);
    start += prefix.size();
    auto end = sse.find('\n', start);
    assert(end != std::string::npos);
    return json::parse(sse.substr(start, end - start));
}

// ===== Role normalization tests =====

static void test_null_role_is_normalized() {
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\",\"role\":null},\"finish_reason\":null}]}\n\n";

    std::string output = lemon::StreamingProxy::normalize_chat_completion_chunk(input);
    auto chunk = parse_first_data_json(output);

    assert(chunk["choices"][0]["delta"]["role"] == "assistant");
    assert(chunk["choices"][0]["delta"]["content"] == "hi");
}

static void test_missing_role_on_content_delta_is_added() {
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"hi\"},\"finish_reason\":null}]}\n\n";

    std::string output = lemon::StreamingProxy::normalize_chat_completion_chunk(input);
    auto chunk = parse_first_data_json(output);

    assert(chunk["choices"][0]["delta"]["role"] == "assistant");
}

static void test_empty_delta_chunk_role_is_not_mutated() {
    // A finish-reason-only delta (no assistant payload) should not get a role added.
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n";
    assert(lemon::StreamingProxy::normalize_chat_completion_chunk(input) == input);
}

static void test_done_marker_and_non_chat_chunks_are_preserved() {
    std::string done = "data: [DONE]\n\n";
    assert(lemon::StreamingProxy::normalize_chat_completion_chunk(done) == done);

    std::string completion =
        "data: {\"object\":\"text_completion\",\"choices\":[{\"index\":0,\"text\":\"hi\"}]}\n\n";
    assert(lemon::StreamingProxy::normalize_chat_completion_chunk(completion) == completion);
}

// ===== Reasoning content normalization tests =====

static void test_reasoning_content_without_content_gets_empty_content() {
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"Let me think...\"},\"finish_reason\":null}]}\n\n";

    std::string output = lemon::StreamingProxy::normalize_chat_completion_chunk(input);
    auto chunk = parse_first_data_json(output);

    assert(chunk["choices"][0]["delta"]["reasoning_content"] == "Let me think...");
    assert(chunk["choices"][0]["delta"]["content"] == "");
}

static void test_reasoning_content_with_null_content_gets_empty_content() {
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"hmm\",\"content\":null},\"finish_reason\":null}]}\n\n";

    std::string output = lemon::StreamingProxy::normalize_chat_completion_chunk(input);
    auto chunk = parse_first_data_json(output);

    assert(chunk["choices"][0]["delta"]["reasoning_content"] == "hmm");
    assert(chunk["choices"][0]["delta"]["content"] == "");
}

static void test_normal_content_delta_gets_role_added() {
    // Content with no role gets role: assistant injected (role normalization)
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}\n\n";
    std::string output = lemon::StreamingProxy::normalize_chat_completion_chunk(input);
    auto chunk = parse_first_data_json(output);
    assert(chunk["choices"][0]["delta"]["content"] == "Hello");
    assert(chunk["choices"][0]["delta"]["role"] == "assistant");
}

static void test_content_and_reasoning_together_is_not_content_mutated() {
    // Both content and reasoning_content present — content should not be touched
    // Role gets added (role normalization) since none was present
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"The\",\"reasoning_content\":\"thinking\"},\"finish_reason\":null}]}\n\n";
    auto output = lemon::StreamingProxy::normalize_chat_completion_chunk(input);
    auto chunk = parse_first_data_json(output);
    assert(chunk["choices"][0]["delta"]["content"] == "The");
    assert(chunk["choices"][0]["delta"]["reasoning_content"] == "thinking");
    assert(chunk["choices"][0]["delta"]["role"] == "assistant");
}

// ===== Combined edge cases =====

static void test_carriage_return_is_preserved() {
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"hmm\"},\"finish_reason\":null}]}\r\n\r\n";
    std::string output = lemon::StreamingProxy::normalize_chat_completion_chunk(input);
    auto chunk = parse_first_data_json(output);
    assert(chunk["choices"][0]["delta"]["reasoning_content"] == "hmm");
    assert(chunk["choices"][0]["delta"]["content"] == "");
    assert(output.size() >= 2 && output[output.size() - 2] == '\r');
}

static void test_multi_choice_handling() {
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":["
        "{\"index\":0,\"delta\":{\"reasoning_content\":\"think\"},\"finish_reason\":null},"
        "{\"index\":1,\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}"
        "]}\n\n";
    std::string output = lemon::StreamingProxy::normalize_chat_completion_chunk(input);
    auto chunk = parse_first_data_json(output);
    // Choice 0: reasoning + content injection + role
    assert(chunk["choices"][0]["delta"]["reasoning_content"] == "think");
    assert(chunk["choices"][0]["delta"]["content"] == "");
    assert(chunk["choices"][0]["delta"]["role"] == "assistant");
    // Choice 1: unchanged (already has content)
    assert(!chunk["choices"][1]["delta"].contains("reasoning_content"));
    assert(chunk["choices"][1]["delta"]["content"] == "Hello");
}

static void test_multiple_lines() {
    std::string input =
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"},\"finish_reason\":null}]}\n"
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"Let me\"},\"finish_reason\":null}]}\n"
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\" think\"},\"finish_reason\":null}]}\n"
        "data: {\"object\":\"chat.completion.chunk\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"The answer is\"},\"finish_reason\":null}]}\n"
        "data: [DONE]\n";

    std::string output = lemon::StreamingProxy::normalize_chat_completion_chunk(input);
    
    std::istringstream stream(output);
    std::string line;
    int line_num = 0;
    while (std::getline(stream, line)) {
        if (line.find("data: ") != 0) continue;
        std::string payload = line.substr(6);
        if (payload == "[DONE]") break;
        
        auto chunk = json::parse(payload);
        auto& delta = chunk["choices"][0]["delta"];
        
        if (line_num == 0) {
            // Role delta — unchanged
            assert(delta["role"] == "assistant");
        } else if (line_num >= 1 && line_num <= 2) {
            // Reasoning deltas — should have content: "" AND role: assistant
            assert(delta.contains("reasoning_content"));
            assert(delta["content"] == "");
            assert(delta["role"] == "assistant");
        } else if (line_num == 3) {
            // Content delta — unchanged
            assert(delta["content"] == "The answer is");
            assert(!delta.contains("reasoning_content"));
        }
        line_num++;
    }
    assert(line_num >= 4);
}

int main() {
    // Role normalization
    test_null_role_is_normalized();
    test_missing_role_on_content_delta_is_added();
    test_empty_delta_chunk_role_is_not_mutated();
    test_done_marker_and_non_chat_chunks_are_preserved();

    // Reasoning content normalization
    test_reasoning_content_without_content_gets_empty_content();
    test_reasoning_content_with_null_content_gets_empty_content();
    test_normal_content_delta_gets_role_added();
    test_content_and_reasoning_together_is_not_content_mutated();

    // Combined edge cases
    test_carriage_return_is_preserved();
    test_multi_choice_handling();
    test_multiple_lines();

    std::cout << "all streaming proxy normalization tests passed\n";
    return 0;
}