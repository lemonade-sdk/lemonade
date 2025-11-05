#include "ryzenai/tool_calls.h"
#include <iostream>
#include <ctime>

namespace ryzenai {

std::pair<std::vector<ToolCall>, std::string> extractToolCalls(const std::string& text) {
    std::vector<ToolCall> tool_calls;
    std::string cleaned_text = text;
    
    std::cout << "[ToolCalls DEBUG] Extracting tool calls from text (" << text.length() << " chars)" << std::endl;
    std::cout << "[ToolCalls DEBUG] Text: " << text.substr(0, std::min(size_t(300), text.length())) << std::endl;
    
    // Pattern for Qwen-style tool calls: <tool_call>...</tool_call>
    // Use [\s\S]*? to match across newlines (. doesn't match newlines in C++ regex by default)
    std::regex tool_call_pattern(R"(<tool_call>([\s\S]*?)</tool_call>)", std::regex::icase | std::regex::ECMAScript);
    
    std::smatch match;
    std::string search_text = text;
    size_t offset = 0;
    
    while (std::regex_search(search_text, match, tool_call_pattern)) {
        std::string tool_call_json = match[1].str();
        
        std::cout << "[ToolCalls DEBUG] Found Qwen-style match, JSON: " << tool_call_json << std::endl;
        
        try {
            // Parse the tool call JSON
            json tool_call_obj = json::parse(tool_call_json);
            
            ToolCall tool_call;
            
            // Extract name
            if (tool_call_obj.contains("name") && tool_call_obj["name"].is_string()) {
                tool_call.name = tool_call_obj["name"];
            } else {
                std::cerr << "[WARNING] Tool call missing 'name' field, skipping" << std::endl;
                search_text = match.suffix();
                offset += match.position() + match.length();
                continue;
            }
            
            // Extract arguments (can be "arguments" or "parameters")
            if (tool_call_obj.contains("arguments")) {
                tool_call.arguments = tool_call_obj["arguments"];
            } else if (tool_call_obj.contains("parameters")) {
                tool_call.arguments = tool_call_obj["parameters"];
            } else {
                std::cerr << "[WARNING] Tool call missing 'arguments' or 'parameters' field, skipping" << std::endl;
                search_text = match.suffix();
                offset += match.position() + match.length();
                continue;
            }
            
            tool_calls.push_back(tool_call);
            
            // Remove the tool call from the cleaned text
            size_t match_pos = offset + match.position();
            size_t match_len = match.length();
            cleaned_text.erase(match_pos, match_len);
            
        } catch (const json::exception& e) {
            std::cerr << "[WARNING] Failed to parse tool call JSON: " << e.what() << std::endl;
        }
        
        search_text = match.suffix();
        offset += match.position() + match.length();
    }
    
    // Also check for [TOOL_CALLS] [...] format (Mistral-style)
    std::regex mistral_pattern(R"(\[TOOL_CALLS\]\s*\[([\s\S]*?)\])", std::regex::icase | std::regex::ECMAScript);
    search_text = cleaned_text;
    offset = 0;
    
    while (std::regex_search(search_text, match, mistral_pattern)) {
        std::string tool_calls_array_json = "[" + match[1].str() + "]";
        
        try {
            json tool_calls_array = json::parse(tool_calls_array_json);
            
            if (tool_calls_array.is_array()) {
                for (const auto& tool_call_obj : tool_calls_array) {
                    ToolCall tool_call;
                    
                    if (tool_call_obj.contains("name") && tool_call_obj["name"].is_string()) {
                        tool_call.name = tool_call_obj["name"];
                    } else {
                        continue;
                    }
                    
                    if (tool_call_obj.contains("arguments")) {
                        tool_call.arguments = tool_call_obj["arguments"];
                    } else if (tool_call_obj.contains("parameters")) {
                        tool_call.arguments = tool_call_obj["parameters"];
                    } else {
                        continue;
                    }
                    
                    tool_calls.push_back(tool_call);
                }
            }
            
            // Remove from cleaned text
            size_t match_pos = offset + match.position();
            size_t match_len = match.length();
            cleaned_text.erase(match_pos, match_len);
            
        } catch (const json::exception& e) {
            std::cerr << "[WARNING] Failed to parse [TOOL_CALLS] JSON: " << e.what() << std::endl;
        }
        
        search_text = match.suffix();
        offset += match.position() + match.length();
    }
    
    // Trim whitespace from cleaned text
    size_t start = cleaned_text.find_first_not_of(" \t\n\r");
    size_t end = cleaned_text.find_last_not_of(" \t\n\r");
    if (start != std::string::npos && end != std::string::npos) {
        cleaned_text = cleaned_text.substr(start, end - start + 1);
    } else if (start == std::string::npos) {
        cleaned_text = "";
    }
    
    std::cout << "[ToolCalls DEBUG] Extracted " << tool_calls.size() << " tool call(s)" << std::endl;
    
    return {tool_calls, cleaned_text};
}

json formatToolCallsForOpenAI(const std::vector<ToolCall>& tool_calls) {
    json openai_tool_calls = json::array();
    
    int index = 0;
    for (const auto& tool_call : tool_calls) {
        // Generate a unique ID for each tool call
        std::string tool_call_id = "call_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(index++);
        
        json openai_tool_call = {
            {"id", tool_call_id},
            {"type", "function"},
            {"function", {
                {"name", tool_call.name},
                {"arguments", tool_call.arguments.dump()}
            }}
        };
        openai_tool_calls.push_back(openai_tool_call);
    }
    
    return openai_tool_calls;
}

} // namespace ryzenai

