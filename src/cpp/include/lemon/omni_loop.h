#pragma once

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>
#include <httplib.h>

namespace lemon {

using json = nlohmann::json;

class Router;
class ModelManager;

struct OmniConfig {
    std::string brain_model;          // The tool-calling LLM model name
    int max_iterations = 25;
    std::vector<std::string> tools;   // Enabled tools: generate_image, describe_image, etc.
    std::string image_model = "SD-Turbo";
    std::string audio_model = "Whisper-Large-v3-Turbo";
    std::string tts_model = "kokoro-v1";
    std::string vision_model = "Qwen3-VL-4B-Instruct-GGUF"; // Dedicated VLM for analyze_image

    // Extensibility: custom system prompt, extra tools, and callback
    std::string system_prompt;          // Custom system prompt (empty = use default)
    json extra_tools;                   // Extra tool definitions (OpenAI format)
    std::string tool_callback_url;      // URL to POST external tool calls to
    int tool_callback_timeout = 30;     // Timeout in seconds for callback
    std::map<std::string, std::string> tool_scripts; // tool name → script command

    static OmniConfig from_json(const json& j, const std::string& model);
};

struct ToolResult {
    std::string tool_call_id;
    std::string tool_name;
    json result_data;       // Full data (images as base64, audio as base64)
    std::string llm_summary; // Compact text for LLM context window
    bool success = true;
};

struct OmniStep {
    int step_number;
    json tool_calls;                // The LLM's tool_calls from the response
    std::vector<ToolResult> results;
};

class OmniLoop {
public:
    OmniLoop(Router* router, ModelManager* model_manager);

    // Non-streaming: returns complete response with omni_steps
    json run(const json& request);

    // Streaming: sends SSE events through DataSink
    void run_stream(const json& request, httplib::DataSink& sink);

private:
    // Build OpenAI-format tool definitions for the enabled tools (+ extra_tools)
    json build_tool_definitions(const OmniConfig& config);

    // Tool executors - each calls Router methods directly
    ToolResult execute_generate_image(const json& args, const std::string& tool_call_id, const OmniConfig& config);
    ToolResult execute_edit_image(const json& args, const std::string& tool_call_id, const OmniConfig& config);
    ToolResult execute_describe_image(const json& args, const std::string& tool_call_id, const OmniConfig& config);
    ToolResult execute_analyze_image(const json& args, const std::string& tool_call_id, const OmniConfig& config);
    ToolResult execute_transcribe_audio(const json& args, const std::string& tool_call_id, const OmniConfig& config);
    ToolResult execute_text_to_speech(const json& args, const std::string& tool_call_id, const OmniConfig& config);

    // Filesystem tools
    ToolResult execute_read_file(const json& args, const std::string& tool_call_id);
    ToolResult execute_write_file(const json& args, const std::string& tool_call_id);
    ToolResult execute_list_directory(const json& args, const std::string& tool_call_id);

    // Web search tool
    ToolResult execute_web_search(const json& args, const std::string& tool_call_id);

    // Model management tools
    ToolResult execute_list_models(const json& args, const std::string& tool_call_id);
    ToolResult execute_load_model(const json& args, const std::string& tool_call_id);

    // Shell tool
    ToolResult execute_run_command(const json& args, const std::string& tool_call_id);

    // Script-based tool: spawn a local process
    ToolResult execute_script_tool(const std::string& script_command, const json& tool_call, const OmniConfig& config);

    // External tool: POST to callback URL
    ToolResult execute_external_tool(const json& tool_call, const OmniConfig& config);

    // Dispatch a tool call to the right executor
    ToolResult execute_tool(const json& tool_call, const OmniConfig& config);

    // Build conversation with system prompt prepended
    json build_conversation(const json& request, const OmniConfig& config);

    // Auto-load a model if not already loaded
    void ensure_model_loaded(const std::string& model_name);

    // Send an SSE event
    void send_sse_event(httplib::DataSink& sink, const std::string& event_type, const json& data);

    Router* router_;
    ModelManager* model_manager_;
};

} // namespace lemon
