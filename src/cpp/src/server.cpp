#include "lemon/server.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/http_client.h"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace lemon {

Server::Server(int port, const std::string& host, const std::string& log_level,
               int ctx_size, bool tray, const std::string& llamacpp_backend)
    : port_(port), host_(host), log_level_(log_level), ctx_size_(ctx_size),
      tray_(tray), llamacpp_backend_(llamacpp_backend), running_(false) {
    
    http_server_ = std::make_unique<httplib::Server>();
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
    // Setup CORS for all routes
    setup_cors();
    
    // Health check
    http_server_->Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
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
    
    http_server_->Post("/api/v1/unload", [this](const httplib::Request& req, httplib::Response& res) {
        handle_unload(req, res);
    });
    
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
}

void Server::run() {
    std::cout << "[Server] Starting on " << host_ << ":" << port_ << std::endl;
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
    res.set_content("{\"status\": \"ok\"}", "application/json");
}

void Server::handle_models(const httplib::Request& req, httplib::Response& res) {
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
        
        // Auto-load model if specified and not loaded
        if (!router_->is_model_loaded() && request_json.contains("model")) {
            std::string model_name = request_json["model"];
            std::cout << "[Server] No model loaded, auto-loading: " << model_name << std::endl;
            
            // Get model info
            if (!model_manager_->model_exists(model_name)) {
                std::cerr << "[Server ERROR] Model not found: " << model_name << std::endl;
                res.status = 404;
                res.set_content("{\"error\": \"Model not found\"}", "application/json");
                return;
            }
            
            auto info = model_manager_->get_model_info(model_name);
            
            // Auto-download if not cached
            if (!model_manager_->is_model_downloaded(model_name)) {
                std::cout << "[Server] Model not downloaded, pulling..." << std::endl;
                model_manager_->download_model(model_name);
            }
            
            // Load the model
            router_->load_model(model_name, info.checkpoint, info.recipe);
            std::cout << "[Server] Model loaded successfully: " << model_name << std::endl;
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();
        
        if (is_streaming) {
            // For streaming, we need to proxy the SSE response directly
            // Get the backend server address
            std::string backend_url = router_->get_backend_address() + "/chat/completions";
            
            // Log the HTTP request
            std::cout << "[Server] POST " << backend_url << " - ";
            
            // Make streaming request using HttpClient
            auto stream_response = utils::HttpClient::post(backend_url, req.body, {{"Content-Type", "application/json"}});
            
            // Complete the log line with status
            std::cout << stream_response.status_code << " " 
                     << (stream_response.status_code == 200 ? "OK" : "Error") << std::endl;
            
            // Forward the SSE response
            res.set_content(stream_response.body, "text/event-stream");
            res.status = stream_response.status_code;
            
            // Parse streaming response to extract telemetry
            // llama-server includes timing data in SSE chunks
            std::istringstream stream(stream_response.body);
            std::string line;
            json last_chunk_with_usage;
            
            while (std::getline(stream, line)) {
                if (line.find("data: ") == 0) {
                    std::string json_str = line.substr(6); // Remove "data: " prefix
                    if (json_str != "[DONE]" && !json_str.empty()) {
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
            }
            
            // Print telemetry if found
            if (!last_chunk_with_usage.empty()) {
                if (last_chunk_with_usage.contains("usage")) {
                    auto usage = last_chunk_with_usage["usage"];
                    std::cout << "\n=== Telemetry ===" << std::endl;
                    if (usage.contains("prompt_tokens")) {
                        std::cout << "Input tokens:  " << usage["prompt_tokens"] << std::endl;
                    }
                    if (usage.contains("completion_tokens")) {
                        std::cout << "Output tokens: " << usage["completion_tokens"] << std::endl;
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
                        std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                                 << ttft_seconds << std::endl;
                    }
                    if (timings.contains("predicted_per_second")) {
                        std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                                 << timings["predicted_per_second"].get<double>() << std::endl;
                    }
                    std::cout << "=================" << std::endl;
                }
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
        std::string model_name = request_json["model"];
        
        model_manager_->download_model(model_name);
        
        nlohmann::json response = {{"status", "success"}, {"model", model_name}};
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_load(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        std::string model_name = request_json["model"];
        
        std::cout << "[Server] Loading model: " << model_name << std::endl;
        
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
        
        // Auto-download if not cached
        if (!model_manager_->is_model_downloaded(model_name)) {
            std::cout << "[Server] Model not downloaded, pulling..." << std::endl;
            model_manager_->download_model(model_name);
        }
        
        router_->load_model(model_name, checkpoint, recipe);
        
        std::cout << "[Server] Model loaded successfully: " << model_name << std::endl;
        
        nlohmann::json response = {
            {"status", "success"},
            {"model", model_name},
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
        router_->unload_model();
        nlohmann::json response = {{"status", "success"}};
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_delete(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        std::string model_name = request_json["model"];
        
        model_manager_->delete_model(model_name);
        
        nlohmann::json response = {{"status", "success"}, {"model", model_name}};
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

