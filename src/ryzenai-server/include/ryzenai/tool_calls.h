#pragma once

#include <string>
#include <vector>
#include <json.hpp>
#include <regex>

namespace ryzenai {

using json = nlohmann::json;

// Extracted tool call structure
struct ToolCall {
    std::string name;
    json arguments;
};

// Extract tool calls from generated text (Qwen format: <tool_call>...</tool_call>)
// Returns: pair of (extracted_tool_calls, cleaned_text_without_tool_calls)
std::pair<std::vector<ToolCall>, std::string> extractToolCalls(const std::string& text);

// Format tool calls in OpenAI API format
json formatToolCallsForOpenAI(const std::vector<ToolCall>& tool_calls);

} // namespace ryzenai

