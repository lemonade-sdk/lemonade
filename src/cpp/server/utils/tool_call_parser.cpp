#include "lemon/utils/tool_call_parser.h"
#include "lemon/utils/aixlog.hpp"
#include <chrono>
#include <sstream>
#include <algorithm>

namespace lemon {
namespace utils {

// ============================================================================
// ToolCallParser Implementation
// ============================================================================

bool ToolCallParser::transform_hermes_response(json& response) {
    try {
        // Check if response has choices array
        if (!response.contains("choices") || !response["choices"].is_array() ||
            response["choices"].empty()) {
            return false;
        }

        auto& choice = response["choices"][0];

        // Check if choice has message with content
        if (!choice.contains("message") || !choice["message"].is_object()) {
            return false;
        }

        auto& message = choice["message"];
        if (!message.contains("content") || !message["content"].is_string()) {
            return false;
        }

        std::string content = message["content"].get<std::string>();

        // Early exit if no Hermes XML markers
        if (!has_hermes_tool_calls(content)) {
            return false;
        }

        // Skip if tool_calls already exists (backwards compatibility)
        if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
            LOG(DEBUG, "ToolCallParser")
                << "Skipping transformation: tool_calls array already exists" << std::endl;
            return false;
        }

        // Extract tool calls and get remaining text
        std::string remaining_text;
        auto tool_calls = extract_hermes_tool_calls(content, remaining_text);

        if (tool_calls.empty()) {
            LOG(DEBUG, "ToolCallParser")
                << "No valid tool calls extracted from Hermes XML" << std::endl;
            return false;
        }

        // Transform the response
        message["content"] = remaining_text;
        message["tool_calls"] = tool_calls;

        // Update finish_reason
        if (choice.contains("finish_reason")) {
            choice["finish_reason"] = "tool_calls";
        }

        LOG(DEBUG, "ToolCallParser")
            << "Transformed " << tool_calls.size() << " Hermes XML tool call(s) to OpenAI format"
            << std::endl;

        return true;

    } catch (const std::exception& e) {
        LOG(ERROR, "ToolCallParser")
            << "Error transforming Hermes response: " << e.what() << std::endl;
        return false;
    }
}

std::vector<json> ToolCallParser::extract_hermes_tool_calls(const std::string& content,
                                                             std::string& remaining_text) {
    std::vector<json> tool_calls;
    remaining_text = content;

    const std::string start_tag = "<tool_call>";
    const std::string end_tag = "</tool_call>";

    size_t pos = 0;
    while (pos < remaining_text.length()) {
        size_t start_pos = remaining_text.find(start_tag, pos);
        if (start_pos == std::string::npos) {
            break;  // No more tool calls
        }

        size_t content_start = start_pos + start_tag.length();
        size_t end_pos = remaining_text.find(end_tag, content_start);

        if (end_pos == std::string::npos) {
            LOG(WARNING, "ToolCallParser")
                << "Unclosed <tool_call> tag found, treating as regular text" << std::endl;
            break;  // Unclosed tag, stop parsing
        }

        // Extract JSON content between tags
        std::string xml_content = remaining_text.substr(content_start, end_pos - content_start);

        // Parse and convert to OpenAI format
        json tool_call = hermes_to_openai_tool_call(xml_content);
        if (!tool_call.is_null()) {
            tool_calls.push_back(tool_call);
        }

        // Remove this tool call block from remaining text
        remaining_text.erase(start_pos, end_pos + end_tag.length() - start_pos);

        // Continue searching from same position (string shifted left)
        pos = start_pos;
    }

    return tool_calls;
}

json ToolCallParser::hermes_to_openai_tool_call(const std::string& xml_content) {
    try {
        // Trim whitespace
        std::string trimmed = xml_content;
        trimmed.erase(0, trimmed.find_first_not_of(" \n\r\t"));
        trimmed.erase(trimmed.find_last_not_of(" \n\r\t") + 1);

        if (trimmed.empty()) {
            LOG(WARNING, "ToolCallParser")
                << "Empty tool call content" << std::endl;
            return json();
        }

        // Parse JSON from XML content
        json hermes_call = json::parse(trimmed);

        // Validate required field: name
        if (!hermes_call.contains("name") || !hermes_call["name"].is_string()) {
            LOG(WARNING, "ToolCallParser")
                << "Tool call missing 'name' field, skipping" << std::endl;
            return json();
        }

        std::string name = hermes_call["name"].get<std::string>();
        if (name.empty()) {
            LOG(WARNING, "ToolCallParser")
                << "Tool call has empty 'name' field, skipping" << std::endl;
            return json();
        }

        // Extract arguments (optional, defaults to empty object)
        json arguments = json::object();
        if (hermes_call.contains("arguments")) {
            if (hermes_call["arguments"].is_object()) {
                arguments = hermes_call["arguments"];
            } else if (hermes_call["arguments"].is_null()) {
                // null arguments → empty object
                arguments = json::object();
            } else {
                LOG(WARNING, "ToolCallParser")
                    << "Tool call arguments not an object, using empty object" << std::endl;
            }
        }

        // Build OpenAI format
        json openai_call = {
            {"id", generate_tool_call_id()},
            {"type", "function"},
            {"function", {
                {"name", name},
                {"arguments", arguments.dump()}  // Must be stringified JSON
            }}
        };

        return openai_call;

    } catch (const json::parse_error& e) {
        LOG(ERROR, "ToolCallParser")
            << "Failed to parse tool call JSON: " << e.what() << std::endl;
        return json();
    } catch (const std::exception& e) {
        LOG(ERROR, "ToolCallParser")
            << "Error parsing tool call: " << e.what() << std::endl;
        return json();
    }
}

std::string ToolCallParser::generate_tool_call_id() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return "call_" + std::to_string(nanos);
}

bool ToolCallParser::has_hermes_tool_calls(const std::string& content) {
    return content.find("<tool_call>") != std::string::npos;
}

std::string ToolCallParser::strip_xml_tags(const std::string& content) {
    std::string result = content;
    std::string dummy_remaining;
    extract_hermes_tool_calls(content, dummy_remaining);
    return dummy_remaining;
}

// ============================================================================
// HermesStreamingParser Implementation
// ============================================================================

HermesStreamingParser::HermesStreamingParser()
    : detected_xml_(false),
      inside_tool_call_(false),
      tool_call_index_(0),
      buffer_size_limit_(100 * 1024),  // 100KB
      chunk_count_(0) {
}

std::string HermesStreamingParser::process_chunk(const std::string& chunk) {
    chunk_count_++;

    // Check for <tool_call> marker
    if (!detected_xml_ && chunk.find("<tool_call>") != std::string::npos) {
        detected_xml_ = true;
        LOG(DEBUG, "HermesStreamingParser")
            << "Detected Hermes XML tool calls in stream" << std::endl;
    }

    // If no XML detected yet, pass through unchanged
    if (!detected_xml_) {
        return chunk;
    }

    std::ostringstream output;
    size_t pos = 0;

    while (pos < chunk.length()) {
        if (inside_tool_call_) {
            // Currently buffering a tool call
            size_t end_pos = chunk.find("</tool_call>", pos);

            if (end_pos != std::string::npos) {
                // Found closing tag - buffer the rest and emit
                buffer_ += chunk.substr(pos, end_pos - pos);
                pos = end_pos + 12;  // Skip "</tool_call>"

                // Emit the complete tool call as a delta
                output << emit_tool_call_delta();

                inside_tool_call_ = false;
                buffer_.clear();
                tool_call_index_++;
            } else {
                // No closing tag yet - buffer entire remainder
                buffer_ += chunk.substr(pos);
                pos = chunk.length();

                if (!check_limits()) {
                    LOG(ERROR, "HermesStreamingParser")
                        << "Buffer limit exceeded, aborting tool call parsing" << std::endl;
                    inside_tool_call_ = false;
                    buffer_.clear();
                    return "";  // Stop processing this stream
                }
            }
        } else {
            // Not in a tool call - look for start tag
            size_t start_pos = chunk.find("<tool_call>", pos);

            if (start_pos != std::string::npos) {
                // Output any text before the tag as normal content delta
                if (start_pos > pos) {
                    std::string text_before = chunk.substr(pos, start_pos - pos);
                    if (!text_before.empty()) {
                        // Emit as content delta (SSE format)
                        output << "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\""
                               << text_before << "\"}}]}\n\n";
                    }
                }

                // Start buffering
                inside_tool_call_ = true;
                pos = start_pos + 11;  // Skip "<tool_call>"
            } else {
                // No tool call in rest of chunk - output as-is
                if (pos < chunk.length()) {
                    output << chunk.substr(pos);
                }
                break;
            }
        }
    }

    return output.str();
}

bool HermesStreamingParser::should_transform() const {
    return detected_xml_;
}

void HermesStreamingParser::reset() {
    buffer_.clear();
    detected_xml_ = false;
    inside_tool_call_ = false;
    tool_call_index_ = 0;
    chunk_count_ = 0;
}

std::string HermesStreamingParser::emit_tool_call_delta() {
    // Parse the buffered tool call
    json tool_call = ToolCallParser::hermes_to_openai_tool_call(buffer_);

    if (tool_call.is_null()) {
        LOG(WARNING, "HermesStreamingParser")
            << "Failed to parse buffered tool call" << std::endl;
        return "";
    }

    // Extract components
    std::string id = tool_call.value("id", "");
    std::string name;
    std::string arguments;

    if (tool_call.contains("function") && tool_call["function"].is_object()) {
        name = tool_call["function"].value("name", "");
        arguments = tool_call["function"].value("arguments", "{}");
    }

    // Build SSE delta events for tool call using proper JSON construction
    std::ostringstream sse;

    // First delta: tool call with id and name
    json delta1;
    delta1["choices"] = json::array();
    delta1["choices"][0]["index"] = 0;
    delta1["choices"][0]["delta"]["tool_calls"] = json::array();
    delta1["choices"][0]["delta"]["tool_calls"][0] = {
        {"index", tool_call_index_},
        {"id", id},
        {"type", "function"},
        {"function", {
            {"name", name},
            {"arguments", ""}
        }}
    };
    sse << "data: " << delta1.dump() << "\n\n";

    // Second delta: arguments (if non-empty)
    if (arguments != "{}") {
        json delta2;
        delta2["choices"] = json::array();
        delta2["choices"][0]["index"] = 0;
        delta2["choices"][0]["delta"]["tool_calls"] = json::array();
        delta2["choices"][0]["delta"]["tool_calls"][0] = {
            {"index", tool_call_index_},
            {"function", {
                {"arguments", arguments}
            }}
        };
        sse << "data: " << delta2.dump() << "\n\n";
    }

    LOG(DEBUG, "HermesStreamingParser")
        << "Emitted tool call delta: " << name << std::endl;

    return sse.str();
}

bool HermesStreamingParser::check_limits() {
    if (buffer_.size() > buffer_size_limit_) {
        return false;
    }

    // Also check if we've been buffering for too long (1000 chunks = safety limit)
    if (chunk_count_ > 1000) {
        return false;
    }

    return true;
}

}  // namespace utils
}  // namespace lemon
