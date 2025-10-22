#include "ryzenai/server.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

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
            // Streaming response
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, comp_req](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false;
                    
                    GenerationParams params;
                    params.max_length = comp_req.max_tokens + 100;  // Add buffer for prompt
                    params.temperature = comp_req.temperature;
                    params.top_p = comp_req.top_p;
                    params.top_k = comp_req.top_k;
                    params.repetition_penalty = comp_req.repeat_penalty;
                    
                    try {
                        int token_count = 0;
                        bool client_disconnected = false;
                        inference_engine_->streamComplete(comp_req.prompt, params, 
                            [&sink, &token_count, &client_disconnected, this](const std::string& token, bool is_final) {
                                if (client_disconnected) {
                                    return;  // Skip this token
                                }
                                
                                json chunk = {
                                    {"id", "chatcmpl-" + std::to_string(std::time(nullptr))},
                                    {"object", "text_completion.chunk"},
                                    {"created", std::time(nullptr)},
                                    {"model", model_id_},
                                    {"choices", {{
                                        {"index", 0},
                                        {"text", token},
                                        {"finish_reason", is_final ? "stop" : nullptr}
                                    }}}
                                };
                                
                                std::string data = "data: " + chunk.dump() + "\n\n";
                                if (!sink.write(data.c_str(), data.size())) {
                                    client_disconnected = true;
                                    std::cerr << "[Server] Client disconnected during streaming" << std::endl;
                                    return;  // Skip remaining tokens
                                }
                                token_count++;
                            }
                        );
                        
                        // Send final done message
                        sink.write("data: [DONE]\n\n", 14);
                        sink.done();  // Signal that we're done streaming
                        std::cout << "[Server] [OK] Streamed " << token_count << " tokens" << std::endl;
                        
                    } catch (const std::exception& e) {
                        std::cerr << "[ERROR] Streaming failed: " << e.what() << std::endl;
                        json error_chunk = createErrorResponse(e.what(), "inference_error");
                        std::string error_data = "data: " + error_chunk.dump() + "\n\n";
                        sink.write(error_data.c_str(), error_data.size());
                        sink.done();  // Signal completion even on error
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
                    {"total_tokens", 0},  // Could calculate actual tokens
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
            // Streaming response
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, prompt, chat_req](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false;
                    
                    GenerationParams params;
                    params.max_length = chat_req.max_tokens + 1000;  // Add buffer for prompt
                    params.temperature = chat_req.temperature;
                    params.top_p = chat_req.top_p;
                    params.top_k = chat_req.top_k;
                    params.repetition_penalty = chat_req.repeat_penalty;
                    
                    try {
                        int token_count = 0;
                        bool client_disconnected = false;
                        inference_engine_->streamComplete(prompt, params, 
                            [&sink, &token_count, &client_disconnected, this](const std::string& token, bool is_final) {
                                try {
                                    std::cout << "[Server] Callback entered, token: '" << token << "'" << std::endl;
                                    if (client_disconnected) {
                                        std::cout << "[Server] Client disconnected, skipping token" << std::endl;
                                        return;  // Skip this token
                                    }
                                    
                                    std::cout << "[Server] Creating JSON chunk..." << std::endl;
                                    json chunk = {
                                        {"id", "chatcmpl-" + std::to_string(std::time(nullptr))},
                                        {"object", "chat.completion.chunk"},
                                        {"created", std::time(nullptr)},
                                        {"model", model_id_},
                                        {"choices", {{
                                            {"index", 0},
                                            {"delta", {{"content", token}}},
                                            {"finish_reason", is_final ? "stop" : nullptr}
                                        }}}
                                    };
                                    
                                    std::cout << "[Server] Dumping JSON..." << std::endl;
                                    std::string data = "data: " + chunk.dump() + "\n\n";
                                    std::cout << "[Server] About to write to sink: " << data.size() << " bytes" << std::endl;
                                    if (!sink.write(data.c_str(), data.size())) {
                                        client_disconnected = true;
                                        std::cerr << "[Server] Client disconnected during streaming" << std::endl;
                                        return;  // Skip remaining tokens
                                    }
                                    std::cout << "[Server] Sink write successful" << std::endl;
                                    token_count++;
                                } catch (const std::exception& e) {
                                    std::cerr << "[Server ERROR] Exception in callback: " << e.what() << std::endl;
                                    client_disconnected = true;
                                } catch (...) {
                                    std::cerr << "[Server ERROR] Unknown exception in callback" << std::endl;
                                    client_disconnected = true;
                                }
                            }
                        );
                        
                        // Send final done message
                        sink.write("data: [DONE]\n\n", 14);
                        sink.done();  // Signal that we're done streaming
                        std::cout << "[Server] [OK] Streamed " << token_count << " tokens" << std::endl;
                        
                    } catch (const std::exception& e) {
                        std::cerr << "[ERROR] Streaming failed: " << e.what() << std::endl;
                        json error_chunk = createErrorResponse(e.what(), "inference_error");
                        std::string error_data = "data: " + error_chunk.dump() + "\n\n";
                        sink.write(error_data.c_str(), error_data.size());
                        sink.done();  // Signal completion even on error
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
                    {"total_tokens", 0},  // Could calculate actual tokens
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

