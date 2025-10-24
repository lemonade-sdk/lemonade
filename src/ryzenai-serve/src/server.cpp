#include "ryzenai/server.h"
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
    
    // Root redirect
    http_server_->Get("/", [this](const httplib::Request&, httplib::Response& res) {
        json response = {
            {"message", "Ryzen AI LLM Server"},
            {"version", "1.0.0"},
            {"model", model_id_},
            {"endpoints", {
                "/health",
                "/v1/completions",
                "/v1/chat/completions"
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
        
        std::cout << "[Server] Completion request (stream=" << comp_req.stream << ")" << std::endl;
        
        if (comp_req.stream) {
            // REAL-TIME STREAMING: Send chunks as tokens are generated
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");
            
            GenerationParams params;
            params.max_length = comp_req.max_tokens + 100;
            params.temperature = comp_req.temperature;
            params.top_p = comp_req.top_p;
            params.top_k = comp_req.top_k;
            params.repetition_penalty = comp_req.repeat_penalty;
            
            std::string prompt = comp_req.prompt;
            std::string model_id = model_id_;
            int token_count = 0;
            
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, prompt, params, model_id, &token_count](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false; // Only run once
                    
                    try {
                        // Generate and send tokens in real-time
                        inference_engine_->streamComplete(prompt, params, 
                            [&sink, model_id, &token_count](const std::string& token, bool is_final) {
                                // WORKAROUND: Creating nlohmann::json objects inside streaming callbacks
                                // causes a crash. This appears to be a memory allocation issue between
                                // the JSON library and the callback context (not related to OGA itself).
                                // Solution: Manually build JSON strings instead of using nlohmann::json objects.
                                std::string escaped_token = token;
                                // Escape special characters for JSON
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
                                
                                std::string finish_reason = is_final ? "\"stop\"" : "null";
                                std::string chunk_json = 
                                    "{\"id\":\"cmpl-" + std::to_string(std::time(nullptr)) + 
                                    "\",\"object\":\"text_completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                    ",\"model\":\"" + model_id + 
                                    "\",\"choices\":[{\"index\":0,\"text\":\"" + escaped_token + 
                                    "\",\"finish_reason\":" + finish_reason + "}]}";
                                
                                std::string chunk_str = "data: " + chunk_json + "\n\n";
                                
                                if (!sink.write(chunk_str.c_str(), chunk_str.size())) {
                                    return; // Client disconnected
                                }
                                token_count++;
                            }
                        );
                        
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
            GenerationParams params;
            params.max_length = comp_req.max_tokens + 100;
            params.temperature = comp_req.temperature;
            params.top_p = comp_req.top_p;
            params.top_k = comp_req.top_k;
            params.repetition_penalty = comp_req.repeat_penalty;
            
            auto start_time = std::chrono::high_resolution_clock::now();
            std::string output = inference_engine_->complete(comp_req.prompt, params);
            auto end_time = std::chrono::high_resolution_clock::now();
            
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Count tokens
            int prompt_tokens = inference_engine_->countTokens(comp_req.prompt);
            int completion_tokens = inference_engine_->countTokens(output);
            int total_tokens = prompt_tokens + completion_tokens;
            
            json response = {
                {"id", "cmpl-" + std::to_string(std::time(nullptr))},
                {"object", "text_completion"},
                {"created", std::time(nullptr)},
                {"model", model_id_},
                {"choices", {{
                    {"index", 0},
                    {"text", output},
                    {"finish_reason", "stop"}
                }}},
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
        
        // Apply the model's chat template
        std::string prompt = inference_engine_->applyChatTemplate(messages_array.dump());
        
        std::cout << "[Server] Chat completion request (stream=" << chat_req.stream << ")" << std::endl;
        
        if (chat_req.stream) {
            // REAL-TIME STREAMING: Send chunks as tokens are generated
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");
            
            GenerationParams params;
            params.max_length = chat_req.max_tokens + 1000;
            params.temperature = chat_req.temperature;
            params.top_p = chat_req.top_p;
            params.top_k = chat_req.top_k;
            params.repetition_penalty = chat_req.repeat_penalty;
            
            std::string model_id = model_id_;
            int token_count = 0;
            
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, prompt, params, model_id, &token_count](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false; // Only run once
                    
                    try {
                        // Generate and send tokens in real-time
                        inference_engine_->streamComplete(prompt, params, 
                            [&sink, model_id, &token_count](const std::string& token, bool is_final) {
                                // WORKAROUND: Creating nlohmann::json objects inside streaming callbacks
                                // causes a crash. This appears to be a memory allocation issue between
                                // the JSON library and the callback context (not related to OGA itself).
                                // Solution: Manually build JSON strings instead of using nlohmann::json objects.
                                std::string escaped_token = token;
                                // Escape special characters for JSON
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
                                
                                std::string finish_reason = is_final ? "\"stop\"" : "null";
                                std::string chunk_json = 
                                    "{\"id\":\"chatcmpl-" + std::to_string(std::time(nullptr)) + 
                                    "\",\"object\":\"chat.completion.chunk\",\"created\":" + std::to_string(std::time(nullptr)) + 
                                    ",\"model\":\"" + model_id + 
                                    "\",\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + escaped_token + 
                                    "\"},\"finish_reason\":" + finish_reason + "}]}";
                                
                                std::string chunk_str = "data: " + chunk_json + "\n\n";
                                
                                if (!sink.write(chunk_str.c_str(), chunk_str.size())) {
                                    return; // Client disconnected
                                }
                                token_count++;
                            }
                        );
                        
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
            GenerationParams params;
            params.max_length = chat_req.max_tokens + 1000;
            params.temperature = chat_req.temperature;
            params.top_p = chat_req.top_p;
            params.top_k = chat_req.top_k;
            params.repetition_penalty = chat_req.repeat_penalty;
            
            auto start_time = std::chrono::high_resolution_clock::now();
            std::string output = inference_engine_->complete(prompt, params);
            auto end_time = std::chrono::high_resolution_clock::now();
            
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
            
            // Count tokens
            int prompt_tokens = inference_engine_->countTokens(prompt);
            int completion_tokens = inference_engine_->countTokens(output);
            int total_tokens = prompt_tokens + completion_tokens;
            
            json response = {
                {"id", "chatcmpl-" + std::to_string(std::time(nullptr))},
                {"object", "chat.completion"},
                {"created", std::time(nullptr)},
                {"model", model_id_},
                {"choices", {{
                    {"index", 0},
                    {"message", {
                        {"role", "assistant"},
                        {"content", output}
                    }},
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

void RyzenAIServer::stop() {
    if (running_) {
        std::cout << "\n[Server] Shutting down..." << std::endl;
        http_server_->stop();
        running_ = false;
    }
}

} // namespace ryzenai

