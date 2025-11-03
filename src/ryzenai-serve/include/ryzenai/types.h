#pragma once

#include <string>
#include <vector>
#include <json.hpp>

namespace ryzenai {

using json = nlohmann::json;

// Command line arguments
struct CommandLineArgs {
    std::string model_path;           // -m, --model (required)
    std::string host = "127.0.0.1";   // --host
    int port = 8080;                  // --port
    std::string mode = "hybrid";      // --mode (npu|hybrid|cpu)
    int ctx_size = 2048;              // --ctx-size
    int threads = 4;                  // --threads
    bool verbose = false;             // --verbose
};

// Chat message structure
struct ChatMessage {
    std::string role;     // "system", "user", "assistant"
    std::string content;
};

// Completion request (OpenAI format)
struct CompletionRequest {
    std::string prompt;
    int max_tokens = 512;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float repeat_penalty = 1.1f;
    bool stream = false;
    bool echo = false;
    std::vector<std::string> stop;
    
    // Parse from JSON
    static CompletionRequest fromJSON(const json& j);
};

// Chat completion request (OpenAI format)
struct ChatCompletionRequest {
    std::vector<ChatMessage> messages;
    int max_tokens = 512;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float repeat_penalty = 1.1f;
    bool stream = false;
    std::vector<std::string> stop;
    json tools;  // Tool definitions (OpenAI format)
    
    // Parse from JSON
    static ChatCompletionRequest fromJSON(const json& j);
    
    // Convert messages to a single prompt string
    std::string toPrompt() const;
};

// Generation parameters for ONNX GenAI
struct GenerationParams {
    int max_length = 2048;
    float temperature = 0.7f;
    float top_p = 0.9f;
    int top_k = 40;
    float repetition_penalty = 1.1f;
    int min_length = 0;
    bool do_sample = true;
    std::vector<std::string> stop_sequences;
};

// Token generation callback for streaming
using StreamCallback = std::function<void(const std::string& token, bool is_final)>;

} // namespace ryzenai

