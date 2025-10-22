#include "lemon/server.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/http_client.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

namespace lemon {

Server::Server(int port, const std::string& host, const std::string& log_level,
               int ctx_size, bool tray, const std::string& llamacpp_backend)
    : port_(port), host_(host), log_level_(log_level), ctx_size_(ctx_size),
      tray_(tray), llamacpp_backend_(llamacpp_backend), running_(false) {
    
    http_server_ = std::make_unique<httplib::Server>();
    
    // CRITICAL: Enable multi-threading so the server can handle concurrent requests
    // Without this, the server is single-threaded and blocks on long operations
    http_server_->new_task_queue = [] { 
        std::cout << "[Server DEBUG] Creating new thread pool with 8 threads" << std::endl;
        return new httplib::ThreadPool(8);
    };
    
    std::cout << "[Server] HTTP server initialized with thread pool (8 threads)" << std::endl;
    
    model_manager_ = std::make_unique<ModelManager>();
    router_ = std::make_unique<Router>(ctx_size, llamacpp_backend, log_level);
    
    if (log_level_ == "debug" || log_level_ == "trace") {
        std::cout << "[Server] Debug logging enabled - subprocess output will be visible" << std::endl;
    }
    
    setup_routes();
}

Server::~Server() {
    stop();
}

void Server::setup_routes() {
    // Add pre-routing handler to log ALL incoming requests
    http_server_->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        std::cout << "[Server PRE-ROUTE] " << req.method << " " << req.path << std::endl;
        std::cout.flush();
        
        // Special handling for POST /api/v1/unload with no body
        // cpp-httplib might reject POST requests without Content-Type, so handle it here
        if (req.method == "POST" && req.path == "/api/v1/unload") {
            std::cout << "[Server PRE-ROUTE] Handling unload in pre-routing" << std::endl;
            handle_unload(req, res);
            return httplib::Server::HandlerResponse::Handled;
        }
        
        return httplib::Server::HandlerResponse::Unhandled;
    });
    
    // Setup CORS for all routes
    setup_cors();
    
    // Health check
    http_server_->Get("/api/v1/health", [this](const httplib::Request& req, httplib::Response& res) {
        handle_health(req, res);
    });
    
    // Models endpoints (both v0 and v1)
    http_server_->Get("/api/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
        handle_models(req, res);
    });
    http_server_->Get("/api/v0/models", [this](const httplib::Request& req, httplib::Response& res) {
        handle_models(req, res);
    });
    
    // Model by ID
    http_server_->Get(R"(/api/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    
    // Chat completions (OpenAI compatible)
    http_server_->Post("/api/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
        handle_chat_completions(req, res);
    });
    http_server_->Post("/api/v0/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
        handle_chat_completions(req, res);
    });
    
    // Completions
    http_server_->Post("/api/v1/completions", [this](const httplib::Request& req, httplib::Response& res) {
        handle_completions(req, res);
    });
    
    // Responses endpoint
    http_server_->Get("/api/v1/responses", [this](const httplib::Request& req, httplib::Response& res) {
        handle_responses(req, res);
    });
    
    // Model management endpoints
    http_server_->Post("/api/v1/pull", [this](const httplib::Request& req, httplib::Response& res) {
        handle_pull(req, res);
    });
    
    http_server_->Post("/api/v1/load", [this](const httplib::Request& req, httplib::Response& res) {
        handle_load(req, res);
    });
    
    // Unload endpoint is handled in pre-routing handler to work around cpp-httplib POST validation
    
    http_server_->Post("/api/v1/delete", [this](const httplib::Request& req, httplib::Response& res) {
        handle_delete(req, res);
    });
    
    http_server_->Post("/api/v1/params", [this](const httplib::Request& req, httplib::Response& res) {
        handle_params(req, res);
    });
    
    // System endpoints
    http_server_->Get("/api/v1/stats", [this](const httplib::Request& req, httplib::Response& res) {
        handle_stats(req, res);
    });
    
    http_server_->Get("/api/v1/system-info", [this](const httplib::Request& req, httplib::Response& res) {
        handle_system_info(req, res);
    });
    
    http_server_->Post("/api/v1/log-level", [this](const httplib::Request& req, httplib::Response& res) {
        handle_log_level(req, res);
    });
    
    // Internal shutdown endpoint (not part of public API)
    http_server_->Post("/internal/shutdown", [this](const httplib::Request& req, httplib::Response& res) {
        handle_shutdown(req, res);
    });
    
    // Test endpoint to verify POST works
    http_server_->Post("/api/v1/test", [](const httplib::Request& req, httplib::Response& res) {
        std::cout << "[Server] TEST POST endpoint hit!" << std::endl;
        res.set_content("{\"test\": \"ok\"}", "application/json");
    });
    
    std::cout << "[Server] Routes setup complete" << std::endl;
}

void Server::setup_static_files() {
    // TODO: Implement static file serving
}

void Server::setup_cors() {
    // Set CORS headers for all responses
    http_server_->set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"}
    });
    
    // Handle preflight OPTIONS requests
    http_server_->Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });
    
    // Catch-all error handler - must be last!
    http_server_->set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        std::cerr << "[Server] Error " << res.status << ": " << req.method << " " << req.path << std::endl;
        
        if (res.status == 404) {
            nlohmann::json error = {
                {"error", {
                    {"message", "The requested endpoint does not exist"},
                    {"type", "not_found"},
                    {"path", req.path}
                }}
            };
            res.set_content(error.dump(), "application/json");
        } else if (res.status == 400) {
            // Log more details about 400 errors
            std::cerr << "[Server] 400 Bad Request details - Body length: " << req.body.length() 
                      << ", Content-Type: " << req.get_header_value("Content-Type") << std::endl;
            // Ensure a response is sent
            if (res.body.empty()) {
                nlohmann::json error = {
                    {"error", {
                        {"message", "Bad request"},
                        {"type", "bad_request"}
                    }}
                };
                res.set_content(error.dump(), "application/json");
            }
        }
    });
}

void Server::run() {
    // Display user-friendly address
    std::string display_host = (host_ == "0.0.0.0") ? "localhost" : host_;
    std::cout << "[Server] Starting on " << display_host << ":" << port_ << std::endl;
    
    // Add request logging for ALL requests
    http_server_->set_logger([](const httplib::Request& req, const httplib::Response& res) {
        std::cout << "[Server] " << req.method << " " << req.path << " - " << res.status << std::endl;
    });
    
    running_ = true;
    http_server_->listen(host_, port_);
}

void Server::stop() {
    if (running_) {
        http_server_->stop();
        running_ = false;
        
        // Unload any loaded model and stop backend servers (only on first stop)
        if (router_) {
            router_->unload_model();
        }
    }
}

bool Server::is_running() const {
    return running_;
}

void Server::handle_health(const httplib::Request& req, httplib::Response& res) {
    auto thread_id = std::this_thread::get_id();
    std::cout << "[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: " << thread_id << ") =====" << std::endl;
    std::cout.flush();
    
    nlohmann::json response = {{"status", "ok"}};
    
    // Add model loaded information like Python implementation
    std::string loaded_checkpoint = router_->get_loaded_checkpoint();
    std::string loaded_model = router_->get_loaded_model();
    
    response["checkpoint_loaded"] = loaded_checkpoint.empty() ? nlohmann::json(nullptr) : loaded_checkpoint;
    response["model_loaded"] = loaded_model.empty() ? nlohmann::json(nullptr) : loaded_model;
    
    res.set_content(response.dump(), "application/json");
    std::cout << "[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: " << thread_id << ") =====" << std::endl;
    std::cout.flush();
}

void Server::handle_models(const httplib::Request& req, httplib::Response& res) {
    std::cout << "[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====" << std::endl;
    std::cout.flush();
    auto all_models = model_manager_->get_supported_models();
    
    // Check if we should show all models (for CLI list command) or only downloaded (OpenAI API behavior)
    bool show_all = req.has_param("show_all") && req.get_param_value("show_all") == "true";
    
    nlohmann::json response;
    response["data"] = nlohmann::json::array();
    response["object"] = "list";
    
    for (const auto& [model_id, model_info] : all_models) {
        bool is_downloaded = model_manager_->is_model_downloaded(model_id);
        
        // Only include downloaded models unless show_all is requested
        if (show_all || is_downloaded) {
            nlohmann::json model_json = {
                {"id", model_id},
                {"object", "model"},
                {"created", 1234567890},
                {"owned_by", "lemonade"},
                {"checkpoint", model_info.checkpoint},
                {"recipe", model_info.recipe}
            };
            
            // Add extra fields when showing all models (for CLI list command)
            if (show_all) {
                model_json["name"] = model_info.model_name;
                model_json["downloaded"] = is_downloaded;
                model_json["labels"] = model_info.labels;
            }
            
            response["data"].push_back(model_json);
        }
    }
    
    res.set_content(response.dump(), "application/json");
    std::cout << "[Server DEBUG] ===== MODELS ENDPOINT RETURNING =====" << std::endl;
    std::cout.flush();
}

void Server::handle_model_by_id(const httplib::Request& req, httplib::Response& res) {
    std::string model_id = req.matches[1];
    
    if (model_manager_->model_exists(model_id)) {
        auto info = model_manager_->get_model_info(model_id);
        nlohmann::json response = {
            {"id", model_id},
            {"name", info.model_name},
            {"checkpoint", info.checkpoint},
            {"recipe", info.recipe}
        };
        res.set_content(response.dump(), "application/json");
    } else {
        res.status = 404;
        res.set_content("{\"error\": \"Model not found\"}", "application/json");
    }
}

void Server::handle_chat_completions(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        
        // Handle model loading/switching
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            std::string loaded_model = router_->get_loaded_model();
            
            // Check if we need to switch models
            if (loaded_model != requested_model) {
                // Unload current model if one is loaded
                if (router_->is_model_loaded()) {
                    std::cout << "[Server] Switching from '" << loaded_model << "' to '" << requested_model << "'" << std::endl;
                    router_->unload_model();
                } else {
                    std::cout << "[Server] No model loaded, auto-loading: " << requested_model << std::endl;
                }
                
                // Get model info
                if (!model_manager_->model_exists(requested_model)) {
                    std::cerr << "[Server ERROR] Model not found: " << requested_model << std::endl;
                    res.status = 404;
                    res.set_content("{\"error\": \"Model not found\"}", "application/json");
                    return;
                }
                
                auto info = model_manager_->get_model_info(requested_model);
                
                // Auto-download if not cached (only for non-FLM models)
                // FLM models are downloaded by FastFlowLMServer using 'flm pull'
                if (info.recipe != "flm" && !model_manager_->is_model_downloaded(requested_model)) {
                    std::cout << "[Server] Model not downloaded, pulling from Hugging Face..." << std::endl;
                    model_manager_->download_model(requested_model);
                }
                
                // Load the requested model (will auto-download FLM models if needed)
                router_->load_model(requested_model, info.checkpoint, info.recipe);
                std::cout << "[Server] Model loaded successfully: " << requested_model << std::endl;
            }
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();
        
        if (is_streaming) {
            try {
                // For streaming, forward chunks in real-time to the client
                std::string backend_url = router_->get_backend_address() + "/chat/completions";
                
                // FLM requires the checkpoint name in the request, not the Lemonade model name
                std::string request_body = req.body;
                if (router_->get_loaded_recipe() == "flm") {
                    auto request_json_copy = request_json;
                    request_json_copy["model"] = router_->get_loaded_checkpoint();
                    request_body = request_json_copy.dump();
                }
                
                // Log the HTTP request
                std::cout << "[Server] POST " << backend_url << " - Streaming" << std::endl;
                
                // CRITICAL: Heap-allocate shared state so it outlives the handler function
                // Using shared_ptr ensures the state lives as long as both threads need it
                struct StreamState {
                    std::queue<std::string> chunk_queue;
                    std::mutex queue_mutex;
                    std::condition_variable queue_cv;
                    bool stream_complete = false;
                    std::string telemetry_buffer;
                    std::unique_ptr<std::thread> curl_thread;
                };
                auto state = std::make_shared<StreamState>();
                
                // Set up streaming response headers
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                res.status = 200;
                
                // Start CURL streaming in a background thread
                state->curl_thread = std::make_unique<std::thread>([state, backend_url, request_body]() {
                    try {
                        auto stream_result = utils::HttpClient::post_stream(
                            backend_url,
                            request_body,
                            [state](const char* data, size_t length) {
                                std::string chunk(data, length);
                                state->telemetry_buffer.append(chunk);
                                
                                {
                                    std::lock_guard<std::mutex> lock(state->queue_mutex);
                                    state->chunk_queue.push(chunk);
                                }
                                state->queue_cv.notify_one();
                                return true;
                            }
                        );
                    } catch (...) {
                        // Error handling
                    }
                    
                    // Signal completion
                    {
                        std::lock_guard<std::mutex> lock(state->queue_mutex);
                        state->stream_complete = true;
                    }
                    state->queue_cv.notify_one();
                });
                
                // Use chunked content provider to stream chunks to client
                // Capture state by value (shared_ptr) so it stays alive
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [state](size_t offset, httplib::DataSink &sink) {
                        bool client_disconnected = false;
                        
                        while (true) {
                            std::unique_lock<std::mutex> lock(state->queue_mutex);
                            
                            // Wait for data or completion
                            state->queue_cv.wait(lock, [&state]() {
                                return !state->chunk_queue.empty() || state->stream_complete;
                            });
                            
                            // Send all available chunks
                            while (!state->chunk_queue.empty()) {
                                std::string chunk = state->chunk_queue.front();
                                state->chunk_queue.pop();
                                lock.unlock();
                                
                                if (!sink.write(chunk.data(), chunk.size())) {
                                    client_disconnected = true;
                                    lock.lock();
                                    break; // Client disconnected, exit inner loop
                                }
                                
                                lock.lock();
                            }
                            
                            // If client disconnected or stream is complete, exit
                            if (client_disconnected || (state->stream_complete && state->chunk_queue.empty())) {
                                lock.unlock();
                                break;
                            }
                        }
                        
                        // CRITICAL: Always join the thread before returning
                        // Otherwise unique_ptr destructor will call std::terminate()
                        if (state->curl_thread && state->curl_thread->joinable()) {
                            state->curl_thread->join();
                        }
                        
                        if (client_disconnected) {
                            std::cout << "[Server] Streaming aborted - client disconnected" << std::endl;
                            return false;
                        }
                        
                        std::cout << "[Server] Streaming completed - 200 OK" << std::endl;
                        
                        // Parse and print telemetry
                        try {
                            std::istringstream stream(state->telemetry_buffer);
                            std::string line;
                            json last_chunk_with_usage;
                        
                            while (std::getline(stream, line)) {
                                // Handle SSE format (data: ...)
                                std::string json_str;
                                if (line.find("data: ") == 0) {
                                    json_str = line.substr(6); // Remove "data: " prefix
                                } else if (line.find("ChatCompletionChunk: ") == 0) {
                                    // FLM debug format
                                    json_str = line.substr(21); // Remove "ChatCompletionChunk: " prefix
                                } else {
                                    // Try parsing as raw JSON
                                    json_str = line;
                                }
                                
                                if (!json_str.empty() && json_str != "[DONE]") {
                                    try {
                                        auto chunk = json::parse(json_str);
                                        // Look for usage or timings in the chunk
                                        if (chunk.contains("usage") || chunk.contains("timings")) {
                                            last_chunk_with_usage = chunk;
                                        }
                                    } catch (...) {
                                        // Skip invalid JSON
                                    }
                                }
                            }
                            
                            // Print telemetry if found
                            if (!last_chunk_with_usage.empty()) {
                                if (last_chunk_with_usage.contains("usage")) {
                                    auto usage = last_chunk_with_usage["usage"];
                                    std::cout << "\n=== Telemetry ===" << std::endl;
                                    
                                    // Input/Output tokens
                                    if (usage.contains("prompt_tokens")) {
                                        std::cout << "Input tokens:  " << usage["prompt_tokens"] << std::endl;
                                    }
                                    if (usage.contains("completion_tokens")) {
                                        std::cout << "Output tokens: " << usage["completion_tokens"] << std::endl;
                                    }
                                    
                                    // TTFT - check FLM format first, then llama.cpp format
                                    if (usage.contains("prefill_duration_ttft")) {
                                        double ttft_seconds = usage["prefill_duration_ttft"].get<double>();
                                        std::cout << "TTFT (s):      " << std::fixed << std::setprecision(3) 
                                                 << ttft_seconds << std::endl;
                                    }
                                    
                                    // TPS - check FLM format first
                                    if (usage.contains("decoding_speed_tps")) {
                                        std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                                                 << usage["decoding_speed_tps"].get<double>() << std::endl;
                                    }
                                    
                                    std::cout << "=================" << std::endl;
                                } else if (last_chunk_with_usage.contains("timings")) {
                                    auto timings = last_chunk_with_usage["timings"];
                                    std::cout << "\n=== Telemetry ===" << std::endl;
                                    if (timings.contains("prompt_n")) {
                                        std::cout << "Input tokens:  " << timings["prompt_n"] << std::endl;
                                    }
                                    if (timings.contains("predicted_n")) {
                                        std::cout << "Output tokens: " << timings["predicted_n"] << std::endl;
                                    }
                                    if (timings.contains("prompt_ms")) {
                                        double ttft_seconds = timings["prompt_ms"].get<double>() / 1000.0;
                                        std::cout << "TTFT (s):      " << std::fixed << std::setprecision(3) 
                                                 << ttft_seconds << std::endl;
                                    }
                                    if (timings.contains("predicted_per_second")) {
                                        std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                                                 << timings["predicted_per_second"].get<double>() << std::endl;
                                    }
                                    std::cout << "=================" << std::endl;
                                }
                            }
                        } catch (const std::exception& e) {
                            // Telemetry parsing is optional, don't error on failure
                        }
                        
                        // Return false to signal "no more data" and stop the provider from being called again
                        return false;
                    }
                );
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\":\"Internal server error during streaming\"}", "application/json");
            }
        } else {
            // Forward to router for non-streaming
            std::string backend_url = router_->get_backend_address() + "/chat/completions";
            
            // Log the HTTP request
            std::cout << "[Server] POST " << backend_url << " - ";
            
            auto response = router_->chat_completion(request_json);
            
            // Complete the log line with status
            std::cout << "200 OK" << std::endl;
            
            res.set_content(response.dump(), "application/json");
            
            // Print telemetry for non-streaming
            // llama-server includes timing data in the response under "timings" field
            if (response.contains("timings")) {
                auto timings = response["timings"];
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (timings.contains("prompt_n")) {
                    std::cout << "Input tokens:  " << timings["prompt_n"] << std::endl;
                }
                if (timings.contains("predicted_n")) {
                    std::cout << "Output tokens: " << timings["predicted_n"] << std::endl;
                }
                if (timings.contains("prompt_ms")) {
                    double ttft_seconds = timings["prompt_ms"].get<double>() / 1000.0;
                    std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                             << ttft_seconds << std::endl;
                }
                if (timings.contains("predicted_per_second")) {
                    std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                             << timings["predicted_per_second"].get<double>() << std::endl;
                }
                std::cout << "=================" << std::endl;
            } else if (response.contains("usage")) {
                // OpenAI format uses "usage" field
                auto usage = response["usage"];
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (usage.contains("prompt_tokens")) {
                    std::cout << "Input tokens:  " << usage["prompt_tokens"] << std::endl;
                }
                if (usage.contains("completion_tokens")) {
                    std::cout << "Output tokens: " << usage["completion_tokens"] << std::endl;
                }
                std::cout << "=================" << std::endl;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[Server ERROR] Chat completion failed: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_completions(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        
        if (!router_->is_model_loaded()) {
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded\"}", "application/json");
            return;
        }
        
        auto response = router_->completion(request_json);
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_responses(const httplib::Request& req, httplib::Response& res) {
    // Return cached responses (stub for now)
    nlohmann::json response = nlohmann::json::array();
    res.set_content(response.dump(), "application/json");
}

void Server::handle_pull(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        std::string model_name = request_json["model_name"];
        
        model_manager_->download_model(model_name);
        
        nlohmann::json response = {{"status", "success"}, {"model_name", model_name}};
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_load(const httplib::Request& req, httplib::Response& res) {
    auto thread_id = std::this_thread::get_id();
    std::cout << "[Server DEBUG] ===== LOAD ENDPOINT ENTERED (Thread: " << thread_id << ") =====" << std::endl;
    std::cout.flush();
    try {
        auto request_json = nlohmann::json::parse(req.body);
        std::string model_name = request_json["model_name"];
        
        std::cout << "[Server] Loading model: " << model_name << std::endl;
        
        // Check if a different model is already loaded
        std::string loaded_model = router_->get_loaded_model();
        if (!loaded_model.empty() && loaded_model != model_name) {
            std::cout << "[Server] Unloading current model: " << loaded_model << std::endl;
            router_->unload_model();
        } else if (loaded_model == model_name) {
            std::cout << "[Server] Model already loaded: " << model_name << std::endl;
            nlohmann::json response = {
                {"status", "success"},
                {"model_name", model_name},
                {"message", "Model already loaded"}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }
        
        // Look up model info from registry if not provided
        std::string checkpoint = request_json.value("checkpoint", "");
        std::string recipe = request_json.value("recipe", "");
        
        if (checkpoint.empty()) {
            // Get model info from model manager
            if (!model_manager_->model_exists(model_name)) {
                std::cerr << "[Server ERROR] Model not found: " << model_name << std::endl;
                res.status = 404;
                res.set_content("{\"error\": \"Model not found\"}", "application/json");
                return;
            }
            
            auto info = model_manager_->get_model_info(model_name);
            checkpoint = info.checkpoint;
            recipe = info.recipe;
        }
        
        // Auto-download if not cached (only for non-FLM models)
        // FLM models are downloaded by FastFlowLMServer using 'flm pull'
        if (recipe != "flm" && !model_manager_->is_model_downloaded(model_name)) {
            std::cout << "[Server] Model not downloaded, pulling from Hugging Face..." << std::endl;
            model_manager_->download_model(model_name);
        }
        
        // Load the model (will auto-download FLM models if needed)
        router_->load_model(model_name, checkpoint, recipe);
        
        std::cout << "[Server] Model loaded successfully: " << model_name << std::endl;
        
        nlohmann::json response = {
            {"status", "success"},
            {"model_name", model_name},
            {"checkpoint", checkpoint},
            {"recipe", recipe}
        };
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        std::cerr << "[Server ERROR] Failed to load model: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_unload(const httplib::Request& req, httplib::Response& res) {
    try {
        std::cout << "[Server] Unload request received" << std::endl;
        std::cout << "[Server] Request method: " << req.method << ", body length: " << req.body.length() << std::endl;
        std::cout << "[Server] Content-Type: " << req.get_header_value("Content-Type") << std::endl;
        
        // Unload doesn't need any request body
        router_->unload_model();
        
        std::cout << "[Server] Model unloaded successfully" << std::endl;
        nlohmann::json response = {
            {"status", "success"},
            {"message", "Model unloaded successfully"}
        };
        res.status = 200; // Explicitly set success status first
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[Server ERROR] Unload failed: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_delete(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        std::string model_name = request_json["model_name"];
        
        model_manager_->delete_model(model_name);
        
        nlohmann::json response = {{"status", "success"}, {"model_name", model_name}};
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_params(const httplib::Request& req, httplib::Response& res) {
    try {
        // Update model parameters (stub for now)
        nlohmann::json response = {{"status", "success"}};
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_stats(const httplib::Request& req, httplib::Response& res) {
    try {
        auto stats = router_->get_stats();
        res.set_content(stats.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_system_info(const httplib::Request& req, httplib::Response& res) {
    nlohmann::json info = {
        {"os", "windows"},
        {"version", "1.0.0"},
        {"port", port_}
    };
    res.set_content(info.dump(), "application/json");
}

void Server::handle_log_level(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        log_level_ = request_json["level"];
        
        nlohmann::json response = {{"status", "success"}, {"level", log_level_}};
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_shutdown(const httplib::Request& req, httplib::Response& res) {
    std::cout << "[Server] Shutdown request received" << std::endl;
    
    nlohmann::json response = {{"status", "shutting down"}};
    res.set_content(response.dump(), "application/json");
    
    // Stop the server (this will trigger cleanup)
    stop();
}

} // namespace lemon

