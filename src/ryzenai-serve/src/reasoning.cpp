#include "ryzenai/reasoning.h"
#include <iostream>

namespace ryzenai {

ReasoningParseResult parseReasoningContent(const std::string& text) {
    ReasoningParseResult result;
    result.has_reasoning = false;
    result.is_thinking = false;
    
    // Look for </think> tag
    size_t close_pos = text.find("</think>");
    
    if (close_pos == std::string::npos) {
        // No closing tag found
        // Check if there's an unclosed <think> tag
        size_t open_pos = text.find("<think>");
        if (open_pos != std::string::npos) {
            // Found opening tag without closing - still thinking
            result.regular_content = text.substr(0, open_pos);
            result.reasoning_content = text.substr(open_pos + 7); // Skip "<think>"
            result.has_reasoning = !result.reasoning_content.empty();
            result.is_thinking = true;
        } else {
            // No tags at all - all regular content
            result.regular_content = text;
        }
        return result;
    }
    
    // Found closing tag, look for opening tag
    size_t open_pos = text.rfind("<think>", close_pos);
    
    if (open_pos != std::string::npos) {
        // Both tags found
        result.regular_content = text.substr(0, open_pos);
        result.reasoning_content = text.substr(open_pos + 7, close_pos - (open_pos + 7));
        result.regular_content += text.substr(close_pos + 8); // Append content after </think>
        result.has_reasoning = true;
        result.is_thinking = false;
    } else {
        // Only closing tag found (Qwen3-Thinking style)
        // Treat everything before </think> as reasoning
        result.reasoning_content = text.substr(0, close_pos);
        result.regular_content = text.substr(close_pos + 8);
        result.has_reasoning = true;
        result.is_thinking = false;
    }
    
    return result;
}

ReasoningStreamParser::ReasoningStreamParser()
    : in_thinking_(false) {
}

void ReasoningStreamParser::reset() {
    in_thinking_ = false;
    buffer_.clear();
}

bool ReasoningStreamParser::containsOpenTag(const std::string& text) const {
    return text.find("<think>") != std::string::npos;
}

bool ReasoningStreamParser::containsCloseTag(const std::string& text) const {
    return text.find("</think>") != std::string::npos;
}

std::pair<std::string, std::string> ReasoningStreamParser::processTags() {
    std::string reasoning_part;
    std::string content_part;
    
    if (in_thinking_) {
        // Currently in thinking mode, look for closing tag
        size_t close_pos = buffer_.find("</think>");
        if (close_pos != std::string::npos) {
            // Found closing tag
            reasoning_part = buffer_.substr(0, close_pos);
            content_part = buffer_.substr(close_pos + 8); // Skip "</think>"
            buffer_.clear();
            in_thinking_ = false;
        } else {
            // Still thinking, no closing tag yet
            reasoning_part = buffer_;
            buffer_.clear();
        }
    } else {
        // Not in thinking mode, look for opening tag
        size_t open_pos = buffer_.find("<think>");
        if (open_pos != std::string::npos) {
            // Found opening tag
            content_part = buffer_.substr(0, open_pos);
            buffer_ = buffer_.substr(open_pos + 7); // Skip "<think>", keep rest in buffer
            in_thinking_ = true;
            
            // Check if there's a closing tag in the remaining buffer
            size_t close_pos = buffer_.find("</think>");
            if (close_pos != std::string::npos) {
                // Both tags in same chunk
                reasoning_part = buffer_.substr(0, close_pos);
                content_part += buffer_.substr(close_pos + 8);
                buffer_.clear();
                in_thinking_ = false;
            } else {
                // Just opening tag, rest is reasoning
                reasoning_part = buffer_;
                buffer_.clear();
            }
        } else {
            // No tags, all content
            content_part = buffer_;
            buffer_.clear();
        }
    }
    
    return {reasoning_part, content_part};
}

std::pair<std::string, std::string> ReasoningStreamParser::processToken(const std::string& token) {
    // Add token to buffer
    buffer_ += token;
    
    // Check if buffer is large enough to contain tags
    // <think> is 7 chars, </think> is 8 chars
    // We'll process when buffer has at least 8 characters or when we detect a complete tag
    
    if (buffer_.length() >= 8 || containsOpenTag(buffer_) || containsCloseTag(buffer_)) {
        return processTags();
    }
    
    // Buffer too small, might contain partial tag - wait for more tokens
    return {"", ""};
}

std::pair<std::string, std::string> ReasoningStreamParser::flush() {
    // Flush any remaining buffer content
    // This should be called when generation is complete (is_final=true)
    if (buffer_.empty()) {
        return {"", ""};
    }
    
    std::string reasoning_part;
    std::string content_part;
    
    if (in_thinking_) {
        // Still in thinking mode at the end - treat remaining buffer as reasoning
        reasoning_part = buffer_;
    } else {
        // Not in thinking mode - treat remaining buffer as regular content
        content_part = buffer_;
    }
    
    buffer_.clear();
    return {reasoning_part, content_part};
}

} // namespace ryzenai

