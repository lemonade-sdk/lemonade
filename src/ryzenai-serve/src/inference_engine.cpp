#include "ryzenai/inference_engine.h"
#include <ort_genai.h>
#include <ort_genai_c.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <thread>
#include <chrono>

namespace ryzenai {

namespace fs = std::filesystem;

InferenceEngine::InferenceEngine(const std::string& model_path, const std::string& mode)
    : execution_mode_(mode) {
    
    std::cout << "[InferenceEngine] Initializing with model: " << model_path << std::endl;
    std::cout << "[InferenceEngine] Execution mode: " << mode << std::endl;
    
    // Resolve model path (handles Hugging Face cache structure)
    model_path_ = resolveModelPath(model_path);
    if (model_path_ != model_path) {
        std::cout << "[InferenceEngine] Resolved to: " << model_path_ << std::endl;
    }
    
    // Validate model directory
    if (!validateModelDirectory(model_path_)) {
        throw std::runtime_error("Invalid model directory: " + model_path_);
    }
    
    // Detect Ryzen AI version and load config
    loadRaiConfig();
    
    // Setup execution provider
    setupExecutionProvider();
    
    // Load the model
    loadModel();
    
    // Extract model name from path
    model_name_ = fs::path(model_path_).filename().string();
    
    // Load default generation params from genai_config.json
    std::string config_path = model_path_ + "/genai_config.json";
    if (fs::exists(config_path)) {
        try {
            std::ifstream file(config_path);
            json config = json::parse(file);
            
            if (config.contains("search")) {
                json search = config["search"];
                has_search_config_ = true;
                
                // Load defaults from search config (matching Python implementation)
                if (search.contains("max_length") && search["max_length"].is_number()) {
                    default_params_.max_length = search["max_length"];
                }
                if (search.contains("temperature") && search["temperature"].is_number()) {
                    default_params_.temperature = search["temperature"];
                }
                if (search.contains("top_p") && search["top_p"].is_number()) {
                    default_params_.top_p = search["top_p"];
                }
                if (search.contains("top_k") && search["top_k"].is_number()) {
                    default_params_.top_k = search["top_k"];
                }
                if (search.contains("repetition_penalty") && search["repetition_penalty"].is_number()) {
                    default_params_.repetition_penalty = search["repetition_penalty"];
                }
                if (search.contains("do_sample") && search["do_sample"].is_boolean()) {
                    default_params_.do_sample = search["do_sample"];
                }
                
                std::cout << "[InferenceEngine] Loaded search config from genai_config.json" << std::endl;
                std::cout << "  - temperature: " << default_params_.temperature << std::endl;
                std::cout << "  - top_p: " << default_params_.top_p << std::endl;
                std::cout << "  - top_k: " << default_params_.top_k << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] Failed to load search config from genai_config.json: " << e.what() << std::endl;
        }
    }
    
    std::cout << "[InferenceEngine] Model loaded successfully: " << model_name_ << std::endl;
    std::cout << "[InferenceEngine] Max prompt length: " << max_prompt_length_ << " tokens" << std::endl;
}

InferenceEngine::~InferenceEngine() {
    std::cout << "[InferenceEngine] Shutting down" << std::endl;
}

GenerationParams InferenceEngine::getDefaultParams() const {
    return default_params_;
}

std::string InferenceEngine::applyChatTemplate(const std::string& messages_json, const std::string& tools_json) {
    // Parse messages
    json messages = json::parse(messages_json);
    std::ostringstream prompt;
    
    // Check if we have a Qwen-style chat template (contains <|im_start|>)
    bool is_qwen_style = !chat_template_.empty() && 
                         (chat_template_.find("<|im_start|>") != std::string::npos ||
                          chat_template_.find("\\u003c|im_start|\\u003e") != std::string::npos);
    
    // If tools are provided, always use OGA's built-in template (it handles tools properly)
    if (!tools_json.empty()) {
        try {
            const char* template_str = chat_template_.empty() ? nullptr : chat_template_.c_str();
            const char* tools_str = tools_json.c_str();
            
            auto result = tokenizer_->ApplyChatTemplate(
                template_str,
                messages_json.c_str(),
                tools_str,
                true
            );
            
            std::cout << "[InferenceEngine] Applied chat template with tools" << std::endl;
            return std::string(result);
            
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] Failed to apply chat template with tools: " << e.what() << std::endl;
            throw;
        }
    } else if (is_qwen_style) {
        // Use Qwen/ChatML format: <|im_start|>role\ncontent<|im_end|>\n
        for (const auto& msg : messages) {
            std::string role = msg.value("role", "user");
            std::string content = msg.value("content", "");
            
            prompt << "<|im_start|>" << role << "\n"
                   << content << "<|im_end|>\n";
        }
        
        // Add generation prompt for assistant
        prompt << "<|im_start|>assistant\n";
        
        std::cout << "[InferenceEngine] Applied Qwen/ChatML template" << std::endl;
    } else {
        // Try using the OGA's built-in chat template
        try {
            const char* template_str = chat_template_.empty() ? nullptr : chat_template_.c_str();
            
            auto result = tokenizer_->ApplyChatTemplate(
                template_str,
                messages_json.c_str(),
                nullptr,
                true
            );
            
            return std::string(result);
            
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] OGA chat template failed: " << e.what() << std::endl;
            std::cerr << "[WARNING] Using simple fallback template" << std::endl;
            
            // Simple fallback template
            prompt.str("");  // Clear
            for (const auto& msg : messages) {
                std::string role = msg.value("role", "user");
                std::string content = msg.value("content", "");
                
                if (role == "system") {
                    prompt << "System: " << content << "\n\n";
                } else if (role == "user") {
                    prompt << "User: " << content << "\n\n";
                } else if (role == "assistant") {
                    prompt << "Assistant: " << content << "\n\n";
                }
            }
            
            prompt << "Assistant: ";
        }
    }
    
    return prompt.str();
}

std::string InferenceEngine::resolveModelPath(const std::string& path) {
    // If path has a "snapshots" subdirectory (Hugging Face cache structure),
    // automatically find the latest snapshot
    std::string snapshots_dir = path + "/snapshots";
    if (fs::exists(snapshots_dir) && fs::is_directory(snapshots_dir)) {
        std::cout << "[InferenceEngine] Detected Hugging Face cache structure, looking for snapshot..." << std::endl;
        
        // Find the first (and usually only) snapshot directory
        for (const auto& entry : fs::directory_iterator(snapshots_dir)) {
            if (entry.is_directory()) {
                std::string snapshot_path = entry.path().string();
                std::cout << "[InferenceEngine] Found snapshot: " << snapshot_path << std::endl;
                return snapshot_path;
            }
        }
        
        std::cerr << "[ERROR] No snapshot found in: " << snapshots_dir << std::endl;
        return path;
    }
    
    // Otherwise, use the path as-is
    return path;
}

bool InferenceEngine::validateModelDirectory(const std::string& path) {
    if (!fs::exists(path) || !fs::is_directory(path)) {
        std::cerr << "[ERROR] Model path does not exist or is not a directory: " << path << std::endl;
        return false;
    }
    
    // Check for required files (at minimum genai_config.json)
    std::string config_path = path + "/genai_config.json";
    if (!fs::exists(config_path)) {
        std::cerr << "[ERROR] Required file not found: " << config_path << std::endl;
        return false;
    }
    
    return true;
}

std::string InferenceEngine::detectRyzenAIVersion() {
    // Check for Ryzen AI 1.6.0 installation
    std::string ryzenai_path_16 = "C:/Program Files/RyzenAI/1.6.0";
    if (fs::exists(ryzenai_path_16)) {
        return "1.6.0";
    }
    
    // Check for 1.5.0
    std::string ryzenai_path_15 = "C:/Program Files/RyzenAI/1.5.0";
    if (fs::exists(ryzenai_path_15)) {
        return "1.5.0";
    }
    
    // Check environment variable
    const char* version_env = std::getenv("RYZENAI_VERSION");
    if (version_env) {
        return std::string(version_env);
    }
    
    // Default to 1.6.0
    return "1.6.0";
}

void InferenceEngine::loadRaiConfig() {
    // Detect Ryzen AI version
    ryzenai_version_ = detectRyzenAIVersion();
    std::cout << "[InferenceEngine] Ryzen AI version: " << ryzenai_version_ << std::endl;
    
    // Load rai_config.json if it exists
    std::string rai_config_path = model_path_ + "/rai_config.json";
    if (fs::exists(rai_config_path)) {
        try {
            std::ifstream file(rai_config_path);
            json config = json::parse(file);
            
            if (config.contains("max_prompt_length") && 
                config["max_prompt_length"].contains(ryzenai_version_)) {
                max_prompt_length_ = config["max_prompt_length"][ryzenai_version_];
                std::cout << "[InferenceEngine] Loaded max_prompt_length from rai_config.json: " 
                         << max_prompt_length_ << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[WARNING] Failed to parse rai_config.json: " << e.what() << std::endl;
        }
    }
}

void InferenceEngine::setupExecutionProvider() {
    std::cout << "[InferenceEngine] Setting up execution provider for mode: " << execution_mode_ << std::endl;
    
    // Note: Actual execution provider configuration happens in ONNX Runtime GenAI
    // based on the genai_config.json file. This method is mainly for validation.
    
    if (execution_mode_ == "npu") {
        std::cout << "[InferenceEngine] Using NPU (VitisAI) execution provider" << std::endl;
    } else if (execution_mode_ == "hybrid") {
        std::cout << "[InferenceEngine] Using Hybrid (NPU + iGPU) execution provider" << std::endl;
    } else if (execution_mode_ == "cpu") {
        std::cout << "[InferenceEngine] Using CPU execution provider" << std::endl;
    }
}

void InferenceEngine::loadModel() {
    try {
        std::cout << "[InferenceEngine] Loading ONNX model from: " << model_path_ << std::endl;
        
        // Create model using factory method
        model_ = OgaModel::Create(model_path_.c_str());
        
        // Create tokenizer using factory method
        tokenizer_ = OgaTokenizer::Create(*model_);
        
        // Load chat template from tokenizer_config.json
        std::string tokenizer_config_path = model_path_ + "/tokenizer_config.json";
        if (fs::exists(tokenizer_config_path)) {
            try {
                std::ifstream file(tokenizer_config_path);
                json config = json::parse(file);
                if (config.contains("chat_template") && config["chat_template"].is_string()) {
                    chat_template_ = config["chat_template"];
                    std::cout << "[InferenceEngine] Loaded chat template from tokenizer_config.json" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "[WARNING] Failed to load chat template: " << e.what() << std::endl;
            }
        }
        
        std::cout << "[InferenceEngine] Model and tokenizer loaded successfully" << std::endl;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to load model: " + std::string(e.what()));
    }
}

std::vector<int32_t> InferenceEngine::truncatePrompt(const std::vector<int32_t>& input_ids) {
    if (input_ids.size() <= static_cast<size_t>(max_prompt_length_)) {
        return input_ids;
    }
    
    // Truncate from the beginning to keep the most recent context
    size_t truncate_amount = input_ids.size() - max_prompt_length_;
    std::cout << "[WARNING] Prompt exceeds maximum length (" 
              << input_ids.size() << " > " << max_prompt_length_ 
              << "). Truncating " << truncate_amount << " tokens from the beginning."
              << std::endl;
    
    return std::vector<int32_t>(
        input_ids.begin() + truncate_amount, 
        input_ids.end()
    );
}

std::string InferenceEngine::complete(const std::string& prompt, const GenerationParams& params) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    
    try {
        // Tokenize input
        auto sequences = OgaSequences::Create();
        tokenizer_->Encode(prompt.c_str(), *sequences);
        
        // Get token IDs and apply truncation
        const int32_t* input_ids_ptr = sequences->SequenceData(0);
        size_t input_ids_count = sequences->SequenceCount(0);
        std::vector<int32_t> input_ids(input_ids_ptr, input_ids_ptr + input_ids_count);
        input_ids = truncatePrompt(input_ids);
        
        // Create generator params
        auto gen_params = OgaGeneratorParams::Create(*model_);
        // max_length should be prompt_length + max_new_tokens
        // params.max_length is max_new_tokens from the caller
        gen_params->SetSearchOption("max_length", static_cast<int>(input_ids.size()) + params.max_length);
        gen_params->SetSearchOption("temperature", params.temperature);
        gen_params->SetSearchOption("top_p", params.top_p);
        gen_params->SetSearchOption("top_k", static_cast<double>(params.top_k));
        gen_params->SetSearchOption("repetition_penalty", params.repetition_penalty);
        gen_params->SetSearchOptionBool("do_sample", params.do_sample);
        // Lock random_seed to 1 for deterministic behavior (matching Python reference)
        gen_params->SetSearchOption("random_seed", 1.0);
        
        // Generate
        auto generator = OgaGenerator::Create(*model_, *gen_params);
        
        // Set input tokens
        generator->AppendTokens(input_ids.data(), input_ids.size());
        
        std::cout << "[InferenceEngine] Generating tokens..." << std::endl;
        
        while (!generator->IsDone()) {
            generator->GenerateNextToken();
        }
        
        // Get the output
        const int32_t* output_ptr = generator->GetSequenceData(0);
        size_t output_count = generator->GetSequenceCount(0);
        
        // Decode only the newly generated tokens (skip the input prompt)
        std::string result;
        if (output_count > input_ids.size()) {
            auto decoded = tokenizer_->Decode(output_ptr + input_ids.size(), output_count - input_ids.size());
            result = std::string(decoded);
        } else {
            // No new tokens generated
            result = "";
        }
        
        // Apply stop sequences - remove stop sequence and everything after it
        for (const auto& stop_seq : params.stop_sequences) {
            size_t pos = result.find(stop_seq);
            if (pos != std::string::npos) {
                result = result.substr(0, pos);
                break;  // Stop at first match
            }
        }
        
        std::cout << "[InferenceEngine] Generated " << (output_count > input_ids.size() ? output_count - input_ids.size() : 0)
                 << " tokens" << std::endl;
        
        return result;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Inference failed: " + std::string(e.what()));
    }
}

void InferenceEngine::streamComplete(const std::string& prompt, 
                                     const GenerationParams& params,
                                     StreamCallback callback) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    
    try {
        // Tokenize input
        auto sequences = OgaSequences::Create();
        tokenizer_->Encode(prompt.c_str(), *sequences);
        
        // Get token IDs and apply truncation
        const int32_t* input_ids_ptr = sequences->SequenceData(0);
        size_t input_ids_count = sequences->SequenceCount(0);
        std::vector<int32_t> input_ids(input_ids_ptr, input_ids_ptr + input_ids_count);
        input_ids = truncatePrompt(input_ids);
        
        // Create generator params
        auto gen_params = OgaGeneratorParams::Create(*model_);
        // max_length should be prompt_length + max_new_tokens
        // params.max_length is max_new_tokens from the caller
        gen_params->SetSearchOption("max_length", static_cast<int>(input_ids.size()) + params.max_length);
        gen_params->SetSearchOption("temperature", params.temperature);
        gen_params->SetSearchOption("top_p", params.top_p);
        gen_params->SetSearchOption("top_k", static_cast<double>(params.top_k));
        gen_params->SetSearchOption("repetition_penalty", params.repetition_penalty);
        gen_params->SetSearchOptionBool("do_sample", params.do_sample);
        // Lock random_seed to 1 for deterministic behavior (matching Python reference)
        gen_params->SetSearchOption("random_seed", 1.0);
        
        // Generate
        auto generator = OgaGenerator::Create(*model_, *gen_params);
        
        // Set input tokens
        generator->AppendTokens(input_ids.data(), input_ids.size());
        
        std::cout << "[InferenceEngine] Generating tokens (streaming)..." << std::endl;
        
        // Use OgaTokenizerStream for efficient incremental token decoding
        auto tokenizer_stream = OgaTokenizerStream::Create(*tokenizer_);
        
        size_t token_count = 0;
        std::string accumulated_output;  // Track full output for stop sequence detection
        
        while (!generator->IsDone()) {
            generator->GenerateNextToken();
            
            // Get just the new token
            const int32_t* all_tokens = generator->GetSequenceData(0);
            size_t num_tokens = generator->GetSequenceCount(0);
            int32_t new_token = all_tokens[num_tokens - 1];
            
            // Decode incrementally using tokenizer stream (this works!)
            const char* decoded = tokenizer_stream->Decode(new_token);
            if (decoded && decoded[0] != '\0') {
                std::string token_str(decoded);
                
                // Check for stop sequences before accumulating
                bool should_stop = false;
                for (const auto& stop_seq : params.stop_sequences) {
                    // Check if adding this token would complete a stop sequence
                    std::string temp_output = accumulated_output + token_str;
                    if (temp_output.find(stop_seq) != std::string::npos) {
                        should_stop = true;
                        break;
                    }
                }
                
                if (should_stop) {
                    // Stop generation - don't send this token
                    break;
                }
                
                accumulated_output += token_str;
                bool is_final = generator->IsDone();
                callback(token_str, is_final);
            }
            
            token_count++;
        }
        
        std::cout << "[InferenceEngine] Generated " << token_count << " tokens (streaming)" << std::endl;
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Streaming inference failed: " + std::string(e.what()));
    }
}

int InferenceEngine::countTokens(const std::string& text) {
    try {
        auto sequences = OgaSequences::Create();
        tokenizer_->Encode(text.c_str(), *sequences);
        return static_cast<int>(sequences->SequenceCount(0));
    } catch (const std::exception& e) {
        std::cerr << "[WARNING] Failed to count tokens: " << e.what() << std::endl;
        return 0;
    }
}

} // namespace ryzenai

