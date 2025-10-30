#pragma once

#include "types.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

// Forward declarations for ONNX Runtime GenAI
struct OgaModel;
struct OgaTokenizer;
struct OgaGeneratorParams;
struct OgaGenerator;
struct OgaSequences;

namespace ryzenai {

class InferenceEngine {
public:
    InferenceEngine(const std::string& model_path, const std::string& mode);
    ~InferenceEngine();
    
    // Synchronous completion
    std::string complete(const std::string& prompt, const GenerationParams& params);
    
    // Streaming completion
    void streamComplete(const std::string& prompt, 
                       const GenerationParams& params,
                       StreamCallback callback);
    
    // Apply chat template to messages
    std::string applyChatTemplate(const std::string& messages_json, const std::string& tools_json = "");
    
    // Getters
    std::string getModelName() const { return model_name_; }
    std::string getExecutionMode() const { return execution_mode_; }
    int getMaxPromptLength() const { return max_prompt_length_; }
    std::string getRyzenAIVersion() const { return ryzenai_version_; }
    
    // Get default generation params from genai_config.json (if available)
    GenerationParams getDefaultParams() const;
    
    // Token counting
    int countTokens(const std::string& text);
    
private:
    void loadModel();
    void setupExecutionProvider();
    void loadRaiConfig();
    std::string detectRyzenAIVersion();
    std::string resolveModelPath(const std::string& path);
    std::vector<int32_t> truncatePrompt(const std::vector<int32_t>& input_ids);
    bool validateModelDirectory(const std::string& path);
    
    std::unique_ptr<OgaModel> model_;
    std::unique_ptr<OgaTokenizer> tokenizer_;
    
    std::string model_path_;
    std::string model_name_;
    std::string execution_mode_;  // "npu", "hybrid", or "cpu"
    std::string ryzenai_version_;
    std::string chat_template_;  // Chat template from tokenizer_config.json
    int max_prompt_length_ = 2048;  // Default, overridden by rai_config.json
    
    // Default generation params from genai_config.json search section
    GenerationParams default_params_;
    bool has_search_config_ = false;
    
    std::mutex inference_mutex_;  // Protect inference operations
};

} // namespace ryzenai

