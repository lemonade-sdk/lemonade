#include "lemon/utils/tool_call_parser.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace lemon::utils;

class ToolCallParserTest : public ::testing::Test {
protected:
    void SetUp() override {
    }
};

// ============================================================================
// Basic Extraction Tests
// ============================================================================

TEST_F(ToolCallParserTest, SingleToolCall) {
    std::string content = "Here is a tool call: <tool_call>\n{\"name\": \"get_weather\", \"arguments\": {\"city\": \"London\"}}\n</tool_call>";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    ASSERT_EQ(tool_calls.size(), 1);
    EXPECT_EQ(tool_calls[0]["type"], "function");
    EXPECT_EQ(tool_calls[0]["function"]["name"], "get_weather");

    // Check arguments are stringified
    EXPECT_TRUE(tool_calls[0]["function"]["arguments"].is_string());
    auto args = json::parse(tool_calls[0]["function"]["arguments"].get<std::string>());
    EXPECT_EQ(args["city"], "London");

    // Check XML removed from content
    EXPECT_EQ(remaining, "Here is a tool call: ");
}

TEST_F(ToolCallParserTest, MultipleToolCalls) {
    std::string content = "<tool_call>{\"name\": \"func1\", \"arguments\": {}}</tool_call> and <tool_call>{\"name\": \"func2\", \"arguments\": {}}</tool_call>";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    ASSERT_EQ(tool_calls.size(), 2);
    EXPECT_EQ(tool_calls[0]["function"]["name"], "func1");
    EXPECT_EQ(tool_calls[1]["function"]["name"], "func2");
    EXPECT_EQ(remaining, " and ");
}

TEST_F(ToolCallParserTest, MixedContent) {
    std::string content = "Let me help you. <tool_call>{\"name\": \"search\", \"arguments\": {\"query\": \"test\"}}</tool_call> Done!";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    ASSERT_EQ(tool_calls.size(), 1);
    EXPECT_EQ(remaining, "Let me help you.  Done!");
}

TEST_F(ToolCallParserTest, NoToolCalls) {
    std::string content = "Just regular text without any tool calls.";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    EXPECT_EQ(tool_calls.size(), 0);
    EXPECT_EQ(remaining, content);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(ToolCallParserTest, EmptyArguments) {
    std::string content = "<tool_call>{\"name\": \"func\", \"arguments\": {}}</tool_call>";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    ASSERT_EQ(tool_calls.size(), 1);
    auto args = json::parse(tool_calls[0]["function"]["arguments"].get<std::string>());
    EXPECT_TRUE(args.is_object());
    EXPECT_EQ(args.size(), 0);
}

TEST_F(ToolCallParserTest, MissingArguments) {
    std::string content = "<tool_call>{\"name\": \"func\"}</tool_call>";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    ASSERT_EQ(tool_calls.size(), 1);
    auto args = json::parse(tool_calls[0]["function"]["arguments"].get<std::string>());
    EXPECT_TRUE(args.is_object());
    EXPECT_EQ(args.size(), 0);
}

TEST_F(ToolCallParserTest, ComplexNestedArguments) {
    std::string content = R"(<tool_call>
{
  "name": "complex_func",
  "arguments": {
    "nested": {
      "array": [1, 2, 3],
      "object": {"key": "value"}
    }
  }
}
</tool_call>)";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    ASSERT_EQ(tool_calls.size(), 1);
    auto args = json::parse(tool_calls[0]["function"]["arguments"].get<std::string>());
    EXPECT_EQ(args["nested"]["array"][0], 1);
    EXPECT_EQ(args["nested"]["object"]["key"], "value");
}

TEST_F(ToolCallParserTest, UnclosedTag) {
    std::string content = "<tool_call>{\"name\": \"func\"}";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    // Should not extract incomplete tool call
    EXPECT_EQ(tool_calls.size(), 0);
}

TEST_F(ToolCallParserTest, MalformedJSON) {
    std::string content = "<tool_call>{\"name\": \"func\", invalid json}</tool_call>";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    // Should skip malformed JSON
    EXPECT_EQ(tool_calls.size(), 0);
}

TEST_F(ToolCallParserTest, MissingName) {
    std::string content = "<tool_call>{\"arguments\": {\"key\": \"value\"}}</tool_call>";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    // Should skip tool call without name
    EXPECT_EQ(tool_calls.size(), 0);
}

TEST_F(ToolCallParserTest, EmptyName) {
    std::string content = "<tool_call>{\"name\": \"\", \"arguments\": {}}</tool_call>";
    std::string remaining;

    auto tool_calls = ToolCallParser::extract_hermes_tool_calls(content, remaining);

    // Should skip tool call with empty name
    EXPECT_EQ(tool_calls.size(), 0);
}

// ============================================================================
// Response Transformation Tests
// ============================================================================

TEST_F(ToolCallParserTest, TransformCompleteResponse) {
    json response = {
        {"choices", json::array({
            {
                {"message", {
                    {"role", "assistant"},
                    {"content", "Let me search: <tool_call>{\"name\": \"search\", \"arguments\": {\"q\": \"test\"}}</tool_call>"}
                }},
                {"finish_reason", "stop"}
            }
        })}
    };

    bool transformed = ToolCallParser::transform_hermes_response(response);

    EXPECT_TRUE(transformed);

    auto& message = response["choices"][0]["message"];
    EXPECT_TRUE(message.contains("tool_calls"));
    EXPECT_EQ(message["tool_calls"].size(), 1);
    EXPECT_EQ(message["content"], "Let me search: ");
    EXPECT_EQ(response["choices"][0]["finish_reason"], "tool_calls");
}

TEST_F(ToolCallParserTest, NoTransformIfNoXML) {
    json response = {
        {"choices", json::array({
            {
                {"message", {
                    {"role", "assistant"},
                    {"content", "Regular response without tool calls"}
                }},
                {"finish_reason", "stop"}
            }
        })}
    };

    bool transformed = ToolCallParser::transform_hermes_response(response);

    EXPECT_FALSE(transformed);
    EXPECT_FALSE(response["choices"][0]["message"].contains("tool_calls"));
}

TEST_F(ToolCallParserTest, SkipIfToolCallsExist) {
    json response = {
        {"choices", json::array({
            {
                {"message", {
                    {"role", "assistant"},
                    {"content", "<tool_call>{\"name\": \"func\"}</tool_call>"},
                    {"tool_calls", json::array({
                        {{"id", "existing"}, {"type", "function"}}
                    })}
                }},
                {"finish_reason", "tool_calls"}
            }
        })}
    };

    bool transformed = ToolCallParser::transform_hermes_response(response);

    // Should not transform if tool_calls already exists
    EXPECT_FALSE(transformed);
    EXPECT_EQ(response["choices"][0]["message"]["tool_calls"].size(), 1);
    EXPECT_EQ(response["choices"][0]["message"]["tool_calls"][0]["id"], "existing");
}

// ============================================================================
// ID Generation Tests
// ============================================================================

TEST_F(ToolCallParserTest, UniqueIDs) {
    std::string id1 = ToolCallParser::generate_tool_call_id();
    std::string id2 = ToolCallParser::generate_tool_call_id();

    EXPECT_NE(id1, id2);
    EXPECT_TRUE(id1.find("call_") == 0);
    EXPECT_TRUE(id2.find("call_") == 0);
}

// ============================================================================
// Streaming Parser Tests
// ============================================================================

TEST_F(ToolCallParserTest, StreamingNoXML) {
    HermesStreamingParser parser;

    std::string chunk1 = "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n";
    std::string result1 = parser.process_chunk(chunk1);

    EXPECT_EQ(result1, chunk1);
    EXPECT_FALSE(parser.should_transform());
}

TEST_F(ToolCallParserTest, StreamingDetectXML) {
    HermesStreamingParser parser;

    std::string chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"<tool_call>\"}}]}\n\n";
    parser.process_chunk(chunk);

    EXPECT_TRUE(parser.should_transform());
}

TEST_F(ToolCallParserTest, StreamingCompleteToolCallInOneChunk) {
    HermesStreamingParser parser;

    std::string chunk = "data: {\"choices\":[{\"delta\":{\"content\":\"<tool_call>{\\\"name\\\":\\\"func\\\",\\\"arguments\\\":{}}</tool_call>\"}}]}\n\n";
    std::string result = parser.process_chunk(chunk);

    // Should contain tool_calls delta
    EXPECT_TRUE(result.find("tool_calls") != std::string::npos);
    EXPECT_TRUE(result.find("\"func\"") != std::string::npos);
}

TEST_F(ToolCallParserTest, StreamingToolCallAcrossChunks) {
    HermesStreamingParser parser;

    std::string chunk1 = "data: <tool_call>{\"name\":\n\n";
    std::string chunk2 = "data: \"search\",\"arguments\":\n\n";
    std::string chunk3 = "data: {\"q\":\"test\"}}</tool_call>\n\n";

    std::string result1 = parser.process_chunk(chunk1);
    std::string result2 = parser.process_chunk(chunk2);
    std::string result3 = parser.process_chunk(chunk3);

    // First chunks should be buffered (empty output)
    EXPECT_TRUE(result1.empty() || result1.find("tool_calls") == std::string::npos);
    EXPECT_TRUE(result2.empty() || result2.find("tool_calls") == std::string::npos);

    // Final chunk should emit the tool call
    EXPECT_TRUE(result3.find("tool_calls") != std::string::npos);
}

TEST_F(ToolCallParserTest, StreamingReset) {
    HermesStreamingParser parser;

    parser.process_chunk("<tool_call>");
    EXPECT_TRUE(parser.should_transform());

    parser.reset();
    EXPECT_FALSE(parser.should_transform());
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
