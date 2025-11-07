#include "ryzenai/server.h"
#include "ryzenai/tool_calls.h"
#include "ryzenai/reasoning.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <thread>

namespace ryzenai {

RyzenAIServer::RyzenAIServer(const CommandLineArgs& args) 
    : args_(args) {
    
    std::cout << "\n";
    std::cout << "===============================================================\n";
    std::cout << "            Ryzen AI LLM Server                                \n";
    std::cout << "            OpenAI API Compatible                              \n";
    std::cout << "===============================================================\n";
    std::cout << "\n";
    
    // Load the model
    loadModel();
    
    // Create HTTP server
    http_server_ = std::make_unique<httplib::Server>();
    
    // Enable multi-threading for better request handling performance
    http_server_->new_task_queue = [] { 
        std::cout << "[Server] Creating thread pool with 8 threads" << std::endl;
        return new httplib::ThreadPool(8);
    };
    
    std::cout << "[Server] HTTP server initialized with thread pool (8 threads)" << std::endl;
    
    // Setup routes
    setupRoutes();
    
    std::cout << "[Server] Initialization complete\n" << std::endl;
}

RyzenAIServer::~RyzenAIServer() {
    stop();
}

GenerationParams RyzenAIServer::createGenerationParams(int max_tokens, float temperature, float top_p, 
                                                       int top_k, float repeat_penalty, 
                                                       const std::vector<std::string>& stop) const {
    // Start with defaults from genai_config.json (or hardcoded defaults if no config)
    GenerationParams params = inference_engine_->getDefaultParams();
    
    std::cout << "[createGenerationParams] Input max_tokens=" << max_tokens << std::endl;
    std::cout << "[createGenerationParams] Default params.max_length=" << params.max_length << std::endl;
    
    // Always apply user-provided values, regardless of whether they match defaults
    // The request parsing already handles providing defaults when values aren't specified
    params.max_length = max_tokens;
    params.temperature = temperature;
    params.top_p = top_p;
    params.top_k = top_k;
    params.repetition_penalty = repeat_penalty;
    params.stop_sequences = stop;
    
    std::cout << "[createGenerationParams] Final params: max_length=" << params.max_length
              << ", temperature=" << params.temperature 
              << ", top_p=" << params.top_p 
              << ", top_k=" << params.top_k 
              << ", do_sample=" << params.do_sample
              << ", repetition_penalty=" << params.repetition_penalty << std::endl;
    
    return params;
}

void RyzenAIServer::loadModel() {
    std::cout << "[Server] Loading model..." << std::endl;
    std::cout << "[Server] Model path: " << args_.model_path << std::endl;
    std::cout << "[Server] Execution mode: " << args_.mode << std::endl;
    
    try {
        inference_engine_ = std::make_unique<InferenceEngine>(
            args_.model_path,
            args_.mode
        );
        
        model_id_ = extractModelName(args_.model_path);
        
        std::cout << "[Server] [OK] Model loaded: " << model_id_ << std::endl;
        std::cout << "[Server] [OK] Execution mode: " << inference_engine_->getExecutionMode() << std::endl;
        std::cout << "[Server] [OK] Max prompt length: " << inference_engine_->getMaxPromptLength() << " tokens" << std::endl;
        std::cout << "[Server] [OK] Ryzen AI version: " << inference_engine_->getRyzenAIVersion() << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "\n[ERROR] Failed to load model: " << e.what() << std::endl;
        throw;
    }
}

std::string RyzenAIServer::extractModelName(const std::string& model_path) {
    // Extract the last component of the path
    size_t last_slash = model_path.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        return model_path.substr(last_slash + 1);
    }
    return model_path;
}

void RyzenAIServer::setupRoutes() {
    std::cout << "[Server] Setting up routes..." << std::endl;
    
    // Set CORS headers for all responses
    http_server_->set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"}
    });
    
    // Handle OPTIONS requests (CORS preflight)
    http_server_->Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });
    
    // Health endpoint
    http_server_->Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
        handleHealth(req, res);
    });
    
    // Completions endpoint
    http_server_->Post("/v1/completions", [this](const httplib::Request& req, httplib::Response& res) {
        handleCompletions(req, res);
    });
    
    // Chat completions endpoint
    http_server_->Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
        handleChatCompletions(req, res);
    });
    
    // Responses endpoint
    http_server_->Post("/v1/responses", [this](const httplib::Request& req, httplib::Response& res) {
        handleResponses(req, res);
    });
    
    // Root redirect
    http_server_->Get("/", [this](const httplib::Request&, httplib::Response& res) {
        json response = {
            {"message", "Ryzen AI LLM Server"},
            {"version", "1.0.0"},
            {"model", model_id_},
            {"endpoints", {
                "/health",
                "/v1/completions",
                "/v1/chat/completions",
                "/v1/responses"
            }}
        };
        res.set_content(response.dump(2), "application/json");
    });
    
    std::cout << "[Server] [OK] Routes configured" << std::endl;
}

json RyzenAIServer::createErrorResponse(const std::string& message, const std::string& type) {
    return {
        {"error", {
            {"message", message},
            {"type", type}
        }}
    };
}

void RyzenAIServer::handleHealth(const httplib::Request& req, httplib::Response& res) {
    json response = {
        {"status", "ok"},
        {"model", model_id_},
        {"execution_mode", inference_engine_->getExecutionMode()},
        {"model_path", args_.model_path},
        {"max_prompt_length", inference_engine_->getMaxPromptLength()},
        {"ryzenai_version", inference_engine_->getRyzenAIVersion()}
    };
    
    res.set_content(response.dump(2), "application/json");
}

void RyzenAIServer::handleCompletions(const httplib::Request& req, httplib::Response& res) {
    try {
        // Parse request
        json request_json = json::parse(req.body);
        auto comp_req = CompletionRequest::fromJSON(request_json);
        
        if (comp_req.prompt.empty()) {
            res.status = 400;
            res.set_content(createErrorResponse("Missing prompt", "invalid_request").dump(), 
                          "application/json");
            return;
        }
        
        std::cout << "[Server] Completion request (stream=" << comp_req.stream 
                  << ", echo=" << comp_req.echo
                  << ", temperature=" << comp_req.temperature 
                  << ", top_p=" << comp_req.top_p 
                  << ", top_k=" << comp_req.top_k << ")" << std::endl;
        
        if (comp_req.stream) {
            if (comp_req.echo) {
                std::cout << "[Server] Warning: `echo` parameter is not supported for streaming completions" << std::endl;
            }
            // REAL-TIME STREAMING: Send chunks as tokens are generated
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");
            
            GenerationParams params = createGenerationParams(
                comp_req.max_tokens, comp_req.temperature, comp_req.top_p,
                comp_req.top_k, comp_req.repeat_penalty, comp_req.stop
            );
            
            std::string prompt = comp_req.prompt;
            std::string model_id = model_id_;
            
            // Count prompt tokens before streaming
            int prompt_tokens = inference_engine_->countTokens(prompt);
            
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, prompt, params, model_id, prompt_tokens](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false; // Only run once
                    
                    try {
                        // Track timing for telemetry
                        auto start_time = std::chrono::high_resolution_clock::now();
                        auto first_token_time = start_time;
                        bool first_token_received = false;
                        int token_count = 0;
                        
                        // Create reasoning parser for streaming
                        ReasoningStreamParser reasoning_parser;
                        
                        // Generate and send tokens in real-time
                        inference_engine_->streamComplete(prompt, params, 
                            [&sink, model_id, &token_count, &reasoning_parser, &first_token_received, &first_token_time](const std::string& token, bool is_final) -> bool {
                                // Track time to first token
                                if (!first_token_received && !token.empty()) {
                                    first_token_time = std::chrono::high_resolution_clock::now();
                                    first_token_received = true;
                                }
                                
                                // Process token through reasoning parser
                                auto [reasoning_part, content_part] = reasoning_parser.processToken(token);
                                
                                // Helper function to escape JSON strings
                                auto escapeJson = [](const std::string& str) -> std::string {
                                    std::string escaped = str;
                                    size_t pos = 0;
                                    while ((pos = escaped.find('\\', pos)) != std::string::npos) {
                                        escaped.replace(pos, 1, "\\\\");
                                        pos += 2;
                                    }
                                    pos = 0;
                                    while ((pos = escaped.find('"', pos)) != std::string::npos) {
                                        escaped.replace(pos, 1, "\\\"");
                                        pos += 2;
                                    }
                                    pos = 0;
                                    while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                                        escaped.replace(pos, 1, "\\n");
                                        pos += 2;
                                    }
                                    pos = 0;
                                    while ((pos = escaped.find('\r', pos)) != std::string::npos) {
                                        escaped.replace(pos, 1, "\\r");
                                        pos += 2;
                                    }
                                    return escaped;
                                };
                                
                                // Send reasoning content chunk if present
                                if (!reasoning_part.empty()) {
                                    std::string escaped_reasoning = escapeJson(reasoning_part);
                                    std::string reasoning_chunk_json = 
                                        "{\"id\":\"cmpl-" + std::to_string(std::time(nullptr)) + 
                                        "\",\"object\":\"text_completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                        ",\"model\":\"" + model_id + 
                                        "\",\"choices\":[{\"index\":0,\"reasoning_content\":\"" + escaped_reasoning + 
                                        "\",\"finish_reason\":null}]}";
                                    
                                    std::string reasoning_chunk_str = "data: " + reasoning_chunk_json + "\n\n";
                                    if (!sink.write(reasoning_chunk_str.c_str(), reasoning_chunk_str.size())) {
                                        return false; // Client disconnected, stop generation
                                    }
                                }
                                
                                // Send regular content chunk if present
                                if (!content_part.empty()) {
                                    std::string escaped_content = escapeJson(content_part);
                                    std::string finish_reason = is_final ? "\"stop\"" : "null";
                                    std::string chunk_json = 
                                        "{\"id\":\"cmpl-" + std::to_string(std::time(nullptr)) + 
                                        "\",\"object\":\"text_completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                        ",\"model\":\"" + model_id + 
                                        "\",\"choices\":[{\"index\":0,\"text\":\"" + escaped_content + 
                                        "\",\"finish_reason\":" + finish_reason + "}]}";
                                    
                                    std::string chunk_str = "data: " + chunk_json + "\n\n";
                                    if (!sink.write(chunk_str.c_str(), chunk_str.size())) {
                                        return false; // Client disconnected, stop generation
                                    }
                                }
                                
                                // If this is the final token, flush any remaining buffered content
                                if (is_final) {
                                    auto [final_reasoning, final_content] = reasoning_parser.flush();
                                    
                                    // Send any remaining reasoning content
                                    if (!final_reasoning.empty()) {
                                        std::string escaped_reasoning = escapeJson(final_reasoning);
                                        std::string reasoning_chunk_json = 
                                            "{\"id\":\"cmpl-" + std::to_string(std::time(nullptr)) + 
                                            "\",\"object\":\"text_completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                            ",\"model\":\"" + model_id + 
                                            "\",\"choices\":[{\"index\":0,\"reasoning_content\":\"" + escaped_reasoning + 
                                            "\",\"finish_reason\":null}]}";
                                        
                                        std::string reasoning_chunk_str = "data: " + reasoning_chunk_json + "\n\n";
                                        if (!sink.write(reasoning_chunk_str.c_str(), reasoning_chunk_str.size())) {
                                            return false;
                                        }
                                    }
                                    
                                    // Send any remaining regular content
                                    if (!final_content.empty()) {
                                        std::string escaped_content = escapeJson(final_content);
                                        std::string chunk_json = 
                                            "{\"id\":\"cmpl-" + std::to_string(std::time(nullptr)) + 
                                            "\",\"object\":\"text_completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                            ",\"model\":\"" + model_id + 
                                            "\",\"choices\":[{\"index\":0,\"text\":\"" + escaped_content + 
                                            "\",\"finish_reason\":\"stop\"}]}";
                                        
                                        std::string chunk_str = "data: " + chunk_json + "\n\n";
                                        if (!sink.write(chunk_str.c_str(), chunk_str.size())) {
                                            return false;
                                        }
                                    }
                                }
                                
                                token_count++;
                                return true; // Continue generation
                            }
                        );
                        
                        // After generation completes, do a final flush to catch any remaining buffered content
                        auto [final_reasoning, final_content] = reasoning_parser.flush();
                        
                        // Helper function to escape JSON strings
                        auto escapeJson = [](const std::string& str) -> std::string {
                            std::string escaped = str;
                            size_t pos = 0;
                            while ((pos = escaped.find('\\', pos)) != std::string::npos) {
                                escaped.replace(pos, 1, "\\\\");
                                pos += 2;
                            }
                            pos = 0;
                            while ((pos = escaped.find('"', pos)) != std::string::npos) {
                                escaped.replace(pos, 1, "\\\"");
                                pos += 2;
                            }
                            pos = 0;
                            while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                                escaped.replace(pos, 1, "\\n");
                                pos += 2;
                            }
                            pos = 0;
                            while ((pos = escaped.find('\r', pos)) != std::string::npos) {
                                escaped.replace(pos, 1, "\\r");
                                pos += 2;
                            }
                            return escaped;
                        };
                        
                        // Send any remaining reasoning content
                        if (!final_reasoning.empty()) {
                            std::string escaped_reasoning = escapeJson(final_reasoning);
                            std::string reasoning_chunk_json = 
                                "{\"id\":\"cmpl-" + std::to_string(std::time(nullptr)) + 
                                "\",\"object\":\"text_completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                ",\"model\":\"" + model_id + 
                                "\",\"choices\":[{\"index\":0,\"reasoning_content\":\"" + escaped_reasoning + 
                                "\",\"finish_reason\":null}]}";
                            
                            std::string reasoning_chunk_str = "data: " + reasoning_chunk_json + "\n\n";
                            sink.write(reasoning_chunk_str.c_str(), reasoning_chunk_str.size());
                        }
                        
                        // Send any remaining regular content
                        if (!final_content.empty()) {
                            std::string escaped_content = escapeJson(final_content);
                            std::string chunk_json = 
                                "{\"id\":\"cmpl-" + std::to_string(std::time(nullptr)) + 
                                "\",\"object\":\"text_completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                ",\"model\":\"" + model_id + 
                                "\",\"choices\":[{\"index\":0,\"text\":\"" + escaped_content + 
                                "\",\"finish_reason\":\"stop\"}]}";
                            
                            std::string chunk_str = "data: " + chunk_json + "\n\n";
                            sink.write(chunk_str.c_str(), chunk_str.size());
                        }
                        
                        // Calculate timing metrics
                        auto end_time = std::chrono::high_resolution_clock::now();
                        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                        auto ttft_duration = std::chrono::duration_cast<std::chrono::milliseconds>(first_token_time - start_time);
                        double ttft_seconds = ttft_duration.count() / 1000.0;
                        double total_seconds = total_duration.count() / 1000.0;
                        double tokens_per_second = (token_count > 0 && total_seconds > 0) ? (token_count / total_seconds) : 0.0;
                        
                        // Send final chunk with usage data (for telemetry parsing)
                        std::string usage_chunk = 
                            "data: {\"id\":\"cmpl-" + std::to_string(std::time(nullptr)) + 
                            "\",\"object\":\"text_completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                            ",\"model\":\"" + model_id + 
                            "\",\"choices\":[{\"index\":0,\"text\":\"\",\"finish_reason\":null}]," +
                            "\"usage\":{" +
                                "\"prompt_tokens\":" + std::to_string(prompt_tokens) + "," +
                                "\"completion_tokens\":" + std::to_string(token_count) + "," +
                                "\"total_tokens\":" + std::to_string(prompt_tokens + token_count) + "," +
                                "\"prefill_duration_ttft\":" + std::to_string(ttft_seconds) + "," +
                                "\"decoding_speed_tps\":" + std::to_string(tokens_per_second) +
                            "}}\n\n";
                        sink.write(usage_chunk.c_str(), usage_chunk.size());
                        
                        // Send [DONE] marker
                        const char* done_msg = "data: [DONE]\n\n";
                        sink.write(done_msg, strlen(done_msg));
                        sink.done();
                        
                        std::cout << "[Server] [OK] Streamed " << token_count << " tokens" << std::endl;
                        
                    } catch (const std::exception& e) {
                        std::cerr << "[ERROR] Streaming failed: " << e.what() << std::endl;
                        json error_chunk = createErrorResponse(e.what(), "inference_error");
                        std::string error_str = "data: " + error_chunk.dump() + "\n\n";
                        sink.write(error_str.c_str(), error_str.size());
                        sink.done();
                    }
                    
                    return false;
                }
            );
            
        } else {
            // Non-streaming response
            GenerationParams params = createGenerationParams(
                comp_req.max_tokens, comp_req.temperature, comp_req.top_p,
                comp_req.top_k, comp_req.repeat_penalty, comp_req.stop
            );
            
            auto start_time = std::chrono::high_resolution_clock::now();
            std::string output = inference_engine_->complete(comp_req.prompt, params);
            auto end_time = std::chrono::high_resolution_clock::now();
            
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Parse reasoning content from output
            auto reasoning_result = parseReasoningContent(output);
            std::string content = reasoning_result.regular_content;
            std::string reasoning_content = reasoning_result.reasoning_content;
            
            if (reasoning_result.has_reasoning) {
                std::cout << "[Server] Extracted reasoning content (" << reasoning_content.length() << " chars)" << std::endl;
            }
            
            // If echo=True, prepend the prompt to the output (matching Python reference)
            std::string final_text = comp_req.echo ? (comp_req.prompt + content) : content;
            
            // Count tokens
            int prompt_tokens = inference_engine_->countTokens(comp_req.prompt);
            int completion_tokens = inference_engine_->countTokens(output);
            int total_tokens = prompt_tokens + completion_tokens;
            
            // Build choice object
            json choice = {
                {"index", 0},
                {"text", final_text},
                {"finish_reason", "stop"}
            };
            
            // Add reasoning_content if present
            if (reasoning_result.has_reasoning && !reasoning_content.empty()) {
                choice["reasoning_content"] = reasoning_content;
            }
            
            json response = {
                {"id", "cmpl-" + std::to_string(std::time(nullptr))},
                {"object", "text_completion"},
                {"created", std::time(nullptr)},
                {"model", model_id_},
                {"choices", {choice}},
                {"usage", {
                    {"prompt_tokens", prompt_tokens},
                    {"completion_tokens", completion_tokens},
                    {"total_tokens", total_tokens},
                    {"completion_time_ms", duration.count()}
                }}
            };
            
            std::cout << "[Server] [OK] Completion generated (" << duration.count() << "ms)" << std::endl;
            res.set_content(response.dump(), "application/json");
        }
        
    } catch (const json::exception& e) {
        res.status = 400;
        res.set_content(createErrorResponse("Invalid JSON: " + std::string(e.what()), 
                                          "parse_error").dump(), 
                       "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(createErrorResponse(e.what(), "internal_error").dump(), 
                       "application/json");
    }
}

void RyzenAIServer::handleChatCompletions(const httplib::Request& req, httplib::Response& res) {
    try {
        // Parse request
        json request_json = json::parse(req.body);
        auto chat_req = ChatCompletionRequest::fromJSON(request_json);
        
        if (chat_req.messages.empty()) {
            res.status = 400;
            res.set_content(createErrorResponse("Missing messages", "invalid_request").dump(), 
                          "application/json");
            return;
        }
        
        // Convert messages to JSON array for chat template
        json messages_array = json::array();
        for (const auto& msg : chat_req.messages) {
            messages_array.push_back({
                {"role", msg.role},
                {"content", msg.content}
            });
        }
        
        // Apply the model's chat template (with tools if provided)
        std::string tools_json = chat_req.tools.empty() ? "" : chat_req.tools.dump();
        
        std::cout << "[Server] Chat completion request (stream=" << chat_req.stream;
        if (!tools_json.empty()) {
            std::cout << ", with " << chat_req.tools.size() << " tools";
            std::cout << ")" << std::endl;
            std::cout << "[Server DEBUG] Tools JSON: " << tools_json << std::endl;
        } else {
            std::cout << ")" << std::endl;
        }
        
        std::string prompt = inference_engine_->applyChatTemplate(messages_array.dump(), tools_json);
        std::cout << "[Server DEBUG] Generated prompt length: " << prompt.length() << " chars" << std::endl;
        std::cout << "[Server DEBUG] Prompt (first 500 chars): " << prompt.substr(0, std::min(size_t(500), prompt.length())) << std::endl;
        
        if (chat_req.stream) {
            // REAL-TIME STREAMING: Send chunks as tokens are generated
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");
            
            GenerationParams params = createGenerationParams(
                chat_req.max_tokens, chat_req.temperature, chat_req.top_p,
                chat_req.top_k, chat_req.repeat_penalty, chat_req.stop
            );
            
            std::string model_id = model_id_;
            bool has_tools = !chat_req.tools.empty();
            
            // Count prompt tokens before streaming
            int prompt_tokens = inference_engine_->countTokens(prompt);
            
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, prompt, params, model_id, has_tools, prompt_tokens](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false; // Only run once
                    
                    try {
                        // Track timing for telemetry
                        auto start_time = std::chrono::high_resolution_clock::now();
                        auto first_token_time = start_time;
                        bool first_token_received = false;
                        int token_count = 0;
                        
                        // Accumulate full response for tool call extraction
                        std::string full_response;
                        
                        // Create reasoning parser for streaming
                        ReasoningStreamParser reasoning_parser;
                        
                        // Generate and send tokens in real-time
                        inference_engine_->streamComplete(prompt, params, 
                            [&sink, model_id, &token_count, &full_response, &reasoning_parser, &first_token_received, &first_token_time](const std::string& token, bool is_final) -> bool {
                                // Track time to first token
                                if (!first_token_received && !token.empty()) {
                                    first_token_time = std::chrono::high_resolution_clock::now();
                                    first_token_received = true;
                                }
                                
                                // Accumulate for tool call extraction
                                full_response += token;
                                
                                // Process token through reasoning parser
                                auto [reasoning_part, content_part] = reasoning_parser.processToken(token);
                                
                                // Helper function to escape JSON strings
                                auto escapeJson = [](const std::string& str) -> std::string {
                                    std::string escaped = str;
                                    size_t pos = 0;
                                    while ((pos = escaped.find('\\', pos)) != std::string::npos) {
                                        escaped.replace(pos, 1, "\\\\");
                                        pos += 2;
                                    }
                                    pos = 0;
                                    while ((pos = escaped.find('"', pos)) != std::string::npos) {
                                        escaped.replace(pos, 1, "\\\"");
                                        pos += 2;
                                    }
                                    pos = 0;
                                    while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                                        escaped.replace(pos, 1, "\\n");
                                        pos += 2;
                                    }
                                    pos = 0;
                                    while ((pos = escaped.find('\r', pos)) != std::string::npos) {
                                        escaped.replace(pos, 1, "\\r");
                                        pos += 2;
                                    }
                                    return escaped;
                                };
                                
                                // Send reasoning content chunk if present
                                if (!reasoning_part.empty()) {
                                    std::string escaped_reasoning = escapeJson(reasoning_part);
                                    std::string reasoning_chunk_json = 
                                        "{\"id\":\"chatcmpl-" + std::to_string(std::time(nullptr)) + 
                                        "\",\"object\":\"chat.completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                        ",\"model\":\"" + model_id + 
                                        "\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"" + escaped_reasoning + 
                                        "\"},\"finish_reason\":null}]}";
                                    
                                    std::string reasoning_chunk_str = "data: " + reasoning_chunk_json + "\n\n";
                                    if (!sink.write(reasoning_chunk_str.c_str(), reasoning_chunk_str.size())) {
                                        return false; // Client disconnected, stop generation
                                    }
                                }
                                
                                // Send regular content chunk if present
                                if (!content_part.empty()) {
                                    std::string escaped_content = escapeJson(content_part);
                                    std::string finish_reason = is_final ? "\"stop\"" : "null";
                                    std::string chunk_json = 
                                        "{\"id\":\"chatcmpl-" + std::to_string(std::time(nullptr)) + 
                                        "\",\"object\":\"chat.completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                        ",\"model\":\"" + model_id + 
                                        "\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + escaped_content + 
                                        "\"},\"finish_reason\":" + finish_reason + "}]}";
                                    
                                    std::string chunk_str = "data: " + chunk_json + "\n\n";
                                    if (!sink.write(chunk_str.c_str(), chunk_str.size())) {
                                        return false; // Client disconnected, stop generation
                                    }
                                }
                                
                                // If this is the final token, flush any remaining buffered content
                                if (is_final) {
                                    auto [final_reasoning, final_content] = reasoning_parser.flush();
                                    
                                    // Send any remaining reasoning content
                                    if (!final_reasoning.empty()) {
                                        std::string escaped_reasoning = escapeJson(final_reasoning);
                                        std::string reasoning_chunk_json = 
                                            "{\"id\":\"chatcmpl-" + std::to_string(std::time(nullptr)) + 
                                            "\",\"object\":\"chat.completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                            ",\"model\":\"" + model_id + 
                                            "\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"" + escaped_reasoning + 
                                            "\"},\"finish_reason\":null}]}";
                                        
                                        std::string reasoning_chunk_str = "data: " + reasoning_chunk_json + "\n\n";
                                        if (!sink.write(reasoning_chunk_str.c_str(), reasoning_chunk_str.size())) {
                                            return false;
                                        }
                                    }
                                    
                                    // Send any remaining regular content
                                    if (!final_content.empty()) {
                                        std::string escaped_content = escapeJson(final_content);
                                        std::string chunk_json = 
                                            "{\"id\":\"chatcmpl-" + std::to_string(std::time(nullptr)) + 
                                            "\",\"object\":\"chat.completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                            ",\"model\":\"" + model_id + 
                                            "\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + escaped_content + 
                                            "\"},\"finish_reason\":\"stop\"}]}";
                                        
                                        std::string chunk_str = "data: " + chunk_json + "\n\n";
                                        if (!sink.write(chunk_str.c_str(), chunk_str.size())) {
                                            return false;
                                        }
                                    }
                                }
                                
                                token_count++;
                                return true; // Continue generation
                            }
                        );
                        
                        // After generation completes, do a final flush to catch any remaining buffered content
                        // This handles the case where the last few tokens didn't trigger processing due to buffer size
                        auto [final_reasoning, final_content] = reasoning_parser.flush();
                        
                        // Helper function to escape JSON strings (reused from above)
                        auto escapeJson = [](const std::string& str) -> std::string {
                            std::string escaped = str;
                            size_t pos = 0;
                            while ((pos = escaped.find('\\', pos)) != std::string::npos) {
                                escaped.replace(pos, 1, "\\\\");
                                pos += 2;
                            }
                            pos = 0;
                            while ((pos = escaped.find('"', pos)) != std::string::npos) {
                                escaped.replace(pos, 1, "\\\"");
                                pos += 2;
                            }
                            pos = 0;
                            while ((pos = escaped.find('\n', pos)) != std::string::npos) {
                                escaped.replace(pos, 1, "\\n");
                                pos += 2;
                            }
                            pos = 0;
                            while ((pos = escaped.find('\r', pos)) != std::string::npos) {
                                escaped.replace(pos, 1, "\\r");
                                pos += 2;
                            }
                            return escaped;
                        };
                        
                        // Send any remaining reasoning content
                        if (!final_reasoning.empty()) {
                            std::string escaped_reasoning = escapeJson(final_reasoning);
                            std::string reasoning_chunk_json = 
                                "{\"id\":\"chatcmpl-" + std::to_string(std::time(nullptr)) + 
                                "\",\"object\":\"chat.completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                ",\"model\":\"" + model_id + 
                                "\",\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"" + escaped_reasoning + 
                                "\"},\"finish_reason\":null}]}";
                            
                            std::string reasoning_chunk_str = "data: " + reasoning_chunk_json + "\n\n";
                            sink.write(reasoning_chunk_str.c_str(), reasoning_chunk_str.size());
                        }
                        
                        // Send any remaining regular content
                        if (!final_content.empty()) {
                            std::string escaped_content = escapeJson(final_content);
                            std::string chunk_json = 
                                "{\"id\":\"chatcmpl-" + std::to_string(std::time(nullptr)) + 
                                "\",\"object\":\"chat.completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                ",\"model\":\"" + model_id + 
                                "\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + escaped_content + 
                                "\"},\"finish_reason\":\"stop\"}]}";
                            
                            std::string chunk_str = "data: " + chunk_json + "\n\n";
                            sink.write(chunk_str.c_str(), chunk_str.size());
                        }
                        
                        // Extract and send tool calls if tools were provided
                        if (has_tools) {
                            auto [extracted_tool_calls, cleaned_text] = extractToolCalls(full_response);
                            if (!extracted_tool_calls.empty()) {
                                std::cout << "[Server] Extracted " << extracted_tool_calls.size() << " tool call(s) from stream" << std::endl;
                                
                                // Send tool calls as delta chunks
                                for (const auto& tool_call : extracted_tool_calls) {
                                    // Escape arguments for JSON
                                    std::string tool_call_args = tool_call.arguments.dump();
                                    std::string escaped_args = tool_call_args;
                                    size_t pos = 0;
                                    while ((pos = escaped_args.find('\\', pos)) != std::string::npos) {
                                        escaped_args.replace(pos, 1, "\\\\");
                                        pos += 2;
                                    }
                                    pos = 0;
                                    while ((pos = escaped_args.find('"', pos)) != std::string::npos) {
                                        escaped_args.replace(pos, 1, "\\\"");
                                        pos += 2;
                                    }
                                    
                                    std::string tool_call_chunk = 
                                        "data: {\"id\":\"chatcmpl-" + std::to_string(std::time(nullptr)) + 
                                        "\",\"object\":\"chat.completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                        ",\"model\":\"" + model_id + 
                                        "\",\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"-\",\"type\":\"function\",\"function\":{\"name\":\"" + tool_call.name + 
                                        "\",\"arguments\":\"" + escaped_args + "\"}}]},\"finish_reason\":null}]}\n\n";
                                    
                                    sink.write(tool_call_chunk.c_str(), tool_call_chunk.size());
                                }
                            }
                        }
                        
                        // Calculate timing metrics
                        auto end_time = std::chrono::high_resolution_clock::now();
                        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
                        auto ttft_duration = std::chrono::duration_cast<std::chrono::milliseconds>(first_token_time - start_time);
                        double ttft_seconds = ttft_duration.count() / 1000.0;
                        double total_seconds = total_duration.count() / 1000.0;
                        double tokens_per_second = (token_count > 0 && total_seconds > 0) ? (token_count / total_seconds) : 0.0;
                        
                        // Send final chunk with usage data (for telemetry parsing)
                        std::string usage_chunk = 
                            "data: {\"id\":\"chatcmpl-" + std::to_string(std::time(nullptr)) + 
                            "\",\"object\":\"chat.completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                            ",\"model\":\"" + model_id + 
                            "\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":null}]," +
                            "\"usage\":{" +
                                "\"prompt_tokens\":" + std::to_string(prompt_tokens) + "," +
                                "\"completion_tokens\":" + std::to_string(token_count) + "," +
                                "\"total_tokens\":" + std::to_string(prompt_tokens + token_count) + "," +
                                "\"prefill_duration_ttft\":" + std::to_string(ttft_seconds) + "," +
                                "\"decoding_speed_tps\":" + std::to_string(tokens_per_second) +
                            "}}\n\n";
                        sink.write(usage_chunk.c_str(), usage_chunk.size());
                        
                        // Send [DONE] marker
                        const char* done_msg = "data: [DONE]\n\n";
                        sink.write(done_msg, strlen(done_msg));
                        sink.done();
                        
                        std::cout << "[Server] [OK] Streamed " << token_count << " tokens" << std::endl;
                        
                    } catch (const std::exception& e) {
                        std::cerr << "[ERROR] Streaming failed: " << e.what() << std::endl;
                        json error_chunk = createErrorResponse(e.what(), "inference_error");
                        std::string error_str = "data: " + error_chunk.dump() + "\n\n";
                        sink.write(error_str.c_str(), error_str.size());
                        sink.done();
                    }
                    
                    return false;
                }
            );
            
        } else {
            // Non-streaming response
            GenerationParams params = createGenerationParams(
                chat_req.max_tokens, chat_req.temperature, chat_req.top_p,
                chat_req.top_k, chat_req.repeat_penalty, chat_req.stop
            );
            
            auto start_time = std::chrono::high_resolution_clock::now();
            std::string output = inference_engine_->complete(prompt, params);
            auto end_time = std::chrono::high_resolution_clock::now();
            
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Parse reasoning content from output
            auto reasoning_result = parseReasoningContent(output);
            std::string content = reasoning_result.regular_content;
            std::string reasoning_content = reasoning_result.reasoning_content;
            
            if (reasoning_result.has_reasoning) {
                std::cout << "[Server] Extracted reasoning content (" << reasoning_content.length() << " chars)" << std::endl;
            }
            
            // Extract tool calls if tools were provided (from the cleaned content)
            json tool_calls_json = nullptr;
            
            if (!chat_req.tools.empty()) {
                std::cout << "[Server DEBUG] Tools provided, extracting tool calls from output..." << std::endl;
                std::cout << "[Server DEBUG] Content length: " << content.length() << " chars" << std::endl;
                std::cout << "[Server DEBUG] First 200 chars: " << content.substr(0, std::min(size_t(200), content.length())) << std::endl;
                
                auto [extracted_tool_calls, cleaned_text] = extractToolCalls(content);
                std::cout << "[Server DEBUG] Extracted " << extracted_tool_calls.size() << " tool call(s)" << std::endl;
                
                if (!extracted_tool_calls.empty()) {
                    content = cleaned_text;
                    tool_calls_json = formatToolCallsForOpenAI(extracted_tool_calls);
                    std::cout << "[Server] Extracted " << extracted_tool_calls.size() << " tool call(s)" << std::endl;
                } else {
                    std::cout << "[Server DEBUG] No tool calls found in output" << std::endl;
                }
            } else {
                std::cout << "[Server DEBUG] No tools provided in request" << std::endl;
            }
            
            // Count tokens
            int prompt_tokens = inference_engine_->countTokens(prompt);
            int completion_tokens = inference_engine_->countTokens(output);
            int total_tokens = prompt_tokens + completion_tokens;
            
            // Build message object
            json message = {
                {"role", "assistant"},
                {"content", content}
            };
            
            // Add reasoning_content if present
            if (reasoning_result.has_reasoning && !reasoning_content.empty()) {
                message["reasoning_content"] = reasoning_content;
            }
            
            if (!tool_calls_json.is_null()) {
                message["tool_calls"] = tool_calls_json;
            }
            
            json response = {
                {"id", "chatcmpl-" + std::to_string(std::time(nullptr))},
                {"object", "chat.completion"},
                {"created", std::time(nullptr)},
                {"model", model_id_},
                {"choices", {{
                    {"index", 0},
                    {"message", message},
                    {"finish_reason", "stop"}
                }}},
                {"usage", {
                    {"prompt_tokens", prompt_tokens},
                    {"completion_tokens", completion_tokens},
                    {"total_tokens", total_tokens},
                    {"completion_time_ms", duration.count()}
                }}
            };
            
            std::cout << "[Server] [OK] Chat completion generated (" << duration.count() << "ms)" << std::endl;
            res.set_content(response.dump(), "application/json");
        }
        
    } catch (const json::exception& e) {
        res.status = 400;
        res.set_content(createErrorResponse("Invalid JSON: " + std::string(e.what()), 
                                          "parse_error").dump(), 
                       "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(createErrorResponse(e.what(), "internal_error").dump(), 
                       "application/json");
    }
}

void RyzenAIServer::run() {
    running_ = true;
    
    std::string display_host = (args_.host == "0.0.0.0") ? "localhost" : args_.host;
    
    std::cout << "\n";
    std::cout << "===============================================================\n";
    std::cout << "  Server running at: http://" << display_host << ":" << args_.port << "\n";
    std::cout << "===============================================================\n";
    std::cout << "\n";
    std::cout << "Available endpoints:\n";
    std::cout << "  GET  http://" << display_host << ":" << args_.port << "/health\n";
    std::cout << "  POST http://" << display_host << ":" << args_.port << "/v1/completions\n";
    std::cout << "  POST http://" << display_host << ":" << args_.port << "/v1/chat/completions\n";
    std::cout << "\n";
    std::cout << "Press Ctrl+C to stop the server\n";
    std::cout << "===============================================================\n\n";
    
    // Start listening
    if (!http_server_->listen(args_.host, args_.port)) {
        throw std::runtime_error("Failed to start server on " + args_.host + ":" + std::to_string(args_.port));
    }
}

void RyzenAIServer::handleResponses(const httplib::Request& req, httplib::Response& res) {
    try {
        // Parse request
        json request_json = json::parse(req.body);
        
        // Extract parameters (following ResponsesRequest format)
        bool stream = request_json.value("stream", false);
        std::string model = request_json.value("model", model_id_);
        int max_output_tokens = request_json.value("max_output_tokens", 512);
        float temperature = request_json.value("temperature", 1.0f);
        float repeat_penalty = request_json.value("repeat_penalty", 1.0f);
        int top_k = request_json.value("top_k", 40);
        float top_p = request_json.value("top_p", 0.9f);
        
        // Handle input - can be string or array of messages
        std::string prompt;
        if (request_json["input"].is_string()) {
            prompt = request_json["input"].get<std::string>();
        } else if (request_json["input"].is_array()) {
            // Apply chat template to messages array
            prompt = inference_engine_->applyChatTemplate(request_json["input"].dump());
        } else {
            res.status = 400;
            res.set_content(createErrorResponse("Input must be string or messages array", "invalid_request").dump(), 
                          "application/json");
            return;
        }
        
        std::cout << "[Server] Responses request (stream=" << stream << ")" << std::endl;
        
        if (stream) {
            // STREAMING RESPONSE: Use Responses API event format
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");
            
            GenerationParams params = createGenerationParams(
                max_output_tokens, temperature, top_p,
                top_k, repeat_penalty, {}
            );
            
            std::string model_name = model;
            
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, prompt, params, model_name](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false; // Only run once
                    
                    try {
                        // Send response.created event
                        std::string created_time = std::to_string(std::time(nullptr));
                        std::string created_event = 
                            "data: {\"type\":\"response.created\",\"sequence_number\":0,"
                            "\"response\":{\"id\":\"0\",\"model\":\"" + model_name + 
                            "\",\"created_at\":" + created_time + 
                            ",\"object\":\"response\",\"output\":[],"
                            "\"parallel_tool_calls\":true,\"tool_choice\":\"auto\",\"tools\":[]}}\n\n";
                        
                        if (!sink.write(created_event.c_str(), created_event.size())) {
                            std::cout << "[Server] Failed to write created event" << std::endl;
                            return false;
                        }
                        
                        // Accumulate full response for the completed event
                        std::string full_response;
                        
                        // Generate and send tokens in real-time
                        inference_engine_->streamComplete(prompt, params, 
                            [&sink, &full_response](const std::string& token, bool is_final) -> bool {
                                // Escape special characters for JSON
                                std::string escaped_token = token;
                                size_t pos = 0;
                                while ((pos = escaped_token.find('\\', pos)) != std::string::npos) {
                                    escaped_token.replace(pos, 1, "\\\\");
                                    pos += 2;
                                }
                                pos = 0;
                                while ((pos = escaped_token.find('"', pos)) != std::string::npos) {
                                    escaped_token.replace(pos, 1, "\\\"");
                                    pos += 2;
                                }
                                pos = 0;
                                while ((pos = escaped_token.find('\n', pos)) != std::string::npos) {
                                    escaped_token.replace(pos, 1, "\\n");
                                    pos += 2;
                                }
                                pos = 0;
                                while ((pos = escaped_token.find('\r', pos)) != std::string::npos) {
                                    escaped_token.replace(pos, 1, "\\r");
                                    pos += 2;
                                }
                                
                                // Accumulate unescaped token for final response
                                full_response += token;
                                
                                // Send response.output_text.delta event
                                std::string delta_event = 
                                    "data: {\"type\":\"response.output_text.delta\",\"sequence_number\":0,"
                                    "\"content_index\":0,\"delta\":\"" + escaped_token + 
                                    "\",\"item_id\":\"0\",\"output_index\":0}\n\n";
                                
                                if (!sink.write(delta_event.c_str(), delta_event.size())) {
                                    std::cout << "[Server] Client disconnected during streaming" << std::endl;
                                    return false; // Client disconnected, stop generation
                                }
                                return true; // Continue generation
                            }
                        );
                        
                        std::cout << "[Server] Token generation completed, sending final events" << std::endl;
                        
                        // Escape full_response for JSON
                        std::string escaped_full_response = full_response;
                        size_t pos = 0;
                        while ((pos = escaped_full_response.find('\\', pos)) != std::string::npos) {
                            escaped_full_response.replace(pos, 1, "\\\\");
                            pos += 2;
                        }
                        pos = 0;
                        while ((pos = escaped_full_response.find('"', pos)) != std::string::npos) {
                            escaped_full_response.replace(pos, 1, "\\\"");
                            pos += 2;
                        }
                        pos = 0;
                        while ((pos = escaped_full_response.find('\n', pos)) != std::string::npos) {
                            escaped_full_response.replace(pos, 1, "\\n");
                            pos += 2;
                        }
                        pos = 0;
                        while ((pos = escaped_full_response.find('\r', pos)) != std::string::npos) {
                            escaped_full_response.replace(pos, 1, "\\r");
                            pos += 2;
                        }
                        
                        // Send response.completed event
                        std::string completed_time = std::to_string(std::time(nullptr));
                        std::string completed_event = 
                            "data: {\"type\":\"response.completed\",\"sequence_number\":0,"
                            "\"response\":{\"id\":\"0\",\"model\":\"" + model_name + 
                            "\",\"created_at\":" + completed_time + 
                            ",\"object\":\"response\",\"output\":[{\"id\":\"0\",\"content\":[{"
                            "\"type\":\"output_text\",\"text\":\"" + escaped_full_response + 
                            "\",\"annotations\":[]}],\"role\":\"assistant\",\"status\":\"completed\","
                            "\"type\":\"message\"}],\"parallel_tool_calls\":true,"
                            "\"tool_choice\":\"auto\",\"tools\":[]}}\n\n";
                        
                        if (!sink.write(completed_event.c_str(), completed_event.size())) {
                            std::cout << "[Server] Failed to write completed event" << std::endl;
                            return false;
                        }
                        
                        // Send [DONE] marker
                        const char* done_msg = "data: [DONE]\n\n";
                        if (!sink.write(done_msg, strlen(done_msg))) {
                            std::cout << "[Server] Failed to write [DONE] marker" << std::endl;
                            return false;
                        }
                        
                        std::cout << "[Server] Streaming responses completed successfully" << std::endl;
                        return true;
                        
                    } catch (const std::exception& e) {
                        std::cerr << "[Server] Error in streaming responses: " << e.what() << std::endl;
                        std::string error_msg = "data: {\"error\":\"" + std::string(e.what()) + "\"}\n\n";
                        sink.write(error_msg.c_str(), error_msg.size());
                        return false;
                    }
                }
            );
            
        } else {
            // NON-STREAMING RESPONSE
            GenerationParams params = createGenerationParams(
                max_output_tokens, temperature, top_p,
                top_k, repeat_penalty, {}
            );
            
            std::string generated_text = inference_engine_->complete(prompt, params);
            
            // Create Response object
            json response = {
                {"id", "0"},
                {"model", model},
                {"created_at", std::time(nullptr)},
                {"object", "response"},
                {"output", json::array({
                    {
                        {"id", "0"},
                        {"content", json::array({
                            {
                                {"type", "output_text"},
                                {"text", generated_text},
                                {"annotations", json::array()}
                            }
                        })},
                        {"role", "assistant"},
                        {"status", "completed"},
                        {"type", "message"}
                    }
                })},
                {"parallel_tool_calls", true},
                {"tool_choice", "auto"},
                {"tools", json::array()}
            };
            
            res.set_content(response.dump(), "application/json");
            std::cout << "[Server] Non-streaming responses completed" << std::endl;
        }
        
    } catch (const json::exception& e) {
        std::cerr << "[Server] JSON parsing error: " << e.what() << std::endl;
        res.status = 400;
        res.set_content(createErrorResponse(e.what(), "invalid_request").dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[Server] Error in responses: " << e.what() << std::endl;
        res.status = 500;
        res.set_content(createErrorResponse(e.what(), "internal_error").dump(), "application/json");
    }
}

void RyzenAIServer::stop() {
    if (running_) {
        std::cout << "\n[Server] Shutting down..." << std::endl;
        http_server_->stop();
        running_ = false;
    }
}

} // namespace ryzenai

