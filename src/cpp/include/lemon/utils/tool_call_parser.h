#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace lemon {
namespace utils {

/**
 * Utility class for parsing Hermes XML-style tool calls and converting them
 * to OpenAI format.
 *
 * Hermes models may return tool calls wrapped in XML tags:
 * <tool_call>
 * {"name": "get_weather", "arguments": {"city": "London"}}
 * </tool_call>
 *
 * This parser detects such XML blocks, extracts the JSON, and transforms
 * the response to OpenAI's standard tool_calls format.
 */
class ToolCallParser {
public:
    /**
     * Transform a complete chat completion response, converting Hermes XML
     * tool calls to OpenAI format.
     *
     * Modifies the response in place:
     * - Extracts <tool_call>...</tool_call> blocks from message content
     * - Parses JSON within each block
     * - Adds tool_calls array to message
     * - Removes XML tags from content
     * - Sets finish_reason to "tool_calls"
     *
     * @param response JSON response object (modified in place)
     * @return true if transformation was performed, false otherwise
     */
    static bool transform_hermes_response(json& response);

    /**
     * Extract all Hermes tool call blocks from content string.
     *
     * Searches for <tool_call>...</tool_call> markers, extracts and parses
     * the JSON within each, and converts to OpenAI format.
     *
     * @param content String potentially containing XML tool call blocks
     * @param remaining_text Output parameter: content with XML tags removed
     * @return Vector of OpenAI-format tool call JSON objects
     */
    static std::vector<json> extract_hermes_tool_calls(const std::string& content,
                                                       std::string& remaining_text);

    /**
     * Parse a single Hermes tool call XML block and convert to OpenAI format.
     *
     * Input: {"name": "get_weather", "arguments": {"city": "London"}}
     * Output: {
     *   "id": "call_123...",
     *   "type": "function",
     *   "function": {
     *     "name": "get_weather",
     *     "arguments": "{\"city\":\"London\"}"
     *   }
     * }
     *
     * @param xml_content JSON string extracted from <tool_call> tags
     * @return OpenAI-format tool call object, or null if parsing fails
     */
    static json hermes_to_openai_tool_call(const std::string& xml_content);

    /**
     * Generate a unique tool call ID.
     *
     * Format: "call_<nanosecond_timestamp>"
     *
     * @return Unique tool call ID string
     */
    static std::string generate_tool_call_id();

private:
    /**
     * Check if content contains Hermes XML tool call markers.
     *
     * @param content String to search
     * @return true if <tool_call> tag found
     */
    static bool has_hermes_tool_calls(const std::string& content);

    /**
     * Strip <tool_call>...</tool_call> blocks from content.
     *
     * @param content Original content string
     * @return Content with XML blocks removed
     */
    static std::string strip_xml_tags(const std::string& content);
};

/**
 * Stateful parser for handling Hermes XML tool calls in streaming responses.
 *
 * Streaming responses arrive in chunks via SSE, and a single tool call may
 * span multiple chunks. This class buffers incomplete XML blocks until the
 * closing tag is received, then transforms them to OpenAI format.
 *
 * Usage:
 *   HermesStreamingParser parser;
 *   for each SSE chunk:
 *     std::string transformed = parser.process_chunk(chunk);
 *     send(transformed);
 */
class HermesStreamingParser {
public:
    HermesStreamingParser();

    /**
     * Process a single SSE chunk, buffering and transforming Hermes XML as needed.
     *
     * Behavior:
     * - If chunk contains start of <tool_call>, begins buffering
     * - If inside tool call, accumulates to buffer
     * - If closing </tool_call> found, parses complete block and emits
     *   tool_calls delta instead of content
     * - Normal text is passed through unchanged
     *
     * @param chunk Raw SSE chunk string
     * @return Transformed chunk (may be empty if buffering, or contain tool_calls delta)
     */
    std::string process_chunk(const std::string& chunk);

    /**
     * Check if this stream has detected Hermes XML and transformation is active.
     *
     * @return true if XML detected and parser is transforming
     */
    bool should_transform() const;

    /**
     * Reset parser state (for reuse or cleanup).
     */
    void reset();

private:
    std::string buffer_;              ///< Buffer for incomplete tool call XML
    bool detected_xml_;               ///< True if <tool_call> detected in this stream
    bool inside_tool_call_;           ///< True if currently inside a tool call block
    size_t tool_call_index_;          ///< Index of current tool call (for delta events)
    size_t buffer_size_limit_;        ///< Maximum buffer size (100KB)
    size_t chunk_count_;              ///< Number of chunks processed

    /**
     * Parse buffered content when </tool_call> is complete.
     *
     * @return SSE chunk with tool_calls delta event
     */
    std::string emit_tool_call_delta();

    /**
     * Check buffer limits to prevent excessive memory use.
     *
     * @return true if within limits, false if exceeded
     */
    bool check_limits();
};

}  // namespace utils
}  // namespace lemon
