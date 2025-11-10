#include "lemon/server.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/streaming_proxy.h"
#include "lemon/system_info.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lemon {

Server::Server(int port, const std::string& host, const std::string& log_level,
               int ctx_size, bool tray, const std::string& llamacpp_backend)
    : port_(port), host_(host), log_level_(log_level), ctx_size_(ctx_size),
      tray_(tray), llamacpp_backend_(llamacpp_backend), running_(false) {
    
    // Detect log file path (same location as tray uses)
    // NOTE: The ServerManager is responsible for redirecting stdout/stderr to this file
    // This server only READS from the file for the SSE streaming endpoint
#ifdef _WIN32
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    log_file_path_ = std::string(temp_path) + "lemonade-server.log";
#else
    log_file_path_ = "/tmp/lemonade-server.log";
#endif
    
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
        return httplib::Server::HandlerResponse::Unhandled;
    });
    
    // Setup CORS for all routes
    setup_cors();
    
    // Helper lambda to register routes for both v0 and v1
    auto register_get = [this](const std::string& endpoint, 
                               std::function<void(const httplib::Request&, httplib::Response&)> handler) {
        http_server_->Get("/api/v0/" + endpoint, handler);
        http_server_->Get("/api/v1/" + endpoint, handler);
    };
    
    auto register_post = [this](const std::string& endpoint, 
                                std::function<void(const httplib::Request&, httplib::Response&)> handler) {
        http_server_->Post("/api/v0/" + endpoint, handler);
        http_server_->Post("/api/v1/" + endpoint, handler);
        // Also register as GET for HEAD request support (HEAD uses GET handler)
        // Return 405 Method Not Allowed (endpoint exists but wrong method)
        http_server_->Get("/api/v0/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
            res.status = 405;
            res.set_content("{\"error\": \"Method Not Allowed. Use POST for this endpoint\"}", "application/json");
        });
        http_server_->Get("/api/v1/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
            res.status = 405;
            res.set_content("{\"error\": \"Method Not Allowed. Use POST for this endpoint\"}", "application/json");
        });
    };
    
    // Health check
    register_get("health", [this](const httplib::Request& req, httplib::Response& res) {
        handle_health(req, res);
    });
    
    // Models endpoints
    register_get("models", [this](const httplib::Request& req, httplib::Response& res) {
        handle_models(req, res);
    });
    
    // Model by ID (need to register for both versions with regex)
    http_server_->Get(R"(/api/v0/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    http_server_->Get(R"(/api/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    
    // Chat completions (OpenAI compatible)
    register_post("chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
        handle_chat_completions(req, res);
    });
    
    // Completions
    register_post("completions", [this](const httplib::Request& req, httplib::Response& res) {
        handle_completions(req, res);
    });
    
    // Embeddings
    register_post("embeddings", [this](const httplib::Request& req, httplib::Response& res) {
        handle_embeddings(req, res);
    });
    
    // Reranking
    register_post("reranking", [this](const httplib::Request& req, httplib::Response& res) {
        handle_reranking(req, res);
    });
    
    // Responses endpoint
    register_post("responses", [this](const httplib::Request& req, httplib::Response& res) {
        handle_responses(req, res);
    });
    
    // Model management endpoints
    register_post("pull", [this](const httplib::Request& req, httplib::Response& res) {
        handle_pull(req, res);
    });
    
    register_post("load", [this](const httplib::Request& req, httplib::Response& res) {
        handle_load(req, res);
    });
    
    register_post("unload", [this](const httplib::Request& req, httplib::Response& res) {
        handle_unload(req, res);
    });
    
    register_post("delete", [this](const httplib::Request& req, httplib::Response& res) {
        handle_delete(req, res);
    });
    
    register_post("params", [this](const httplib::Request& req, httplib::Response& res) {
        handle_params(req, res);
    });
    
    register_post("add-local-model", [this](const httplib::Request& req, httplib::Response& res) {
        handle_add_local_model(req, res);
    });
    
    // System endpoints
    register_get("stats", [this](const httplib::Request& req, httplib::Response& res) {
        handle_stats(req, res);
    });
    
    register_get("system-info", [this](const httplib::Request& req, httplib::Response& res) {
        handle_system_info(req, res);
    });
    
    register_post("log-level", [this](const httplib::Request& req, httplib::Response& res) {
        handle_log_level(req, res);
    });
    
    // Log streaming endpoint (SSE)
    register_get("logs/stream", [this](const httplib::Request& req, httplib::Response& res) {
        handle_logs_stream(req, res);
    });
    
    // NOTE: /api/v1/halt endpoint removed - use SIGTERM signal instead (like Python server)
    // The stop command now sends termination signal directly to the process
    
    // Internal shutdown endpoint (not part of public API)
    http_server_->Post("/internal/shutdown", [this](const httplib::Request& req, httplib::Response& res) {
        handle_shutdown(req, res);
    });
    
    // Test endpoint to verify POST works
    http_server_->Post("/api/v1/test", [](const httplib::Request& req, httplib::Response& res) {
        std::cout << "[Server] TEST POST endpoint hit!" << std::endl;
        res.set_content("{\"test\": \"ok\"}", "application/json");
    });
    
    // Setup static file serving for web UI
    setup_static_files();
    
    std::cout << "[Server] Routes setup complete" << std::endl;
}

void Server::setup_static_files() {
    // Determine static files directory (relative to executable)
    std::string static_dir = utils::get_resource_path("resources/static");
    
    // Root path redirects to web UI
    http_server_->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/webapp.html");
    });
    
    // Special handler for webapp.html to replace template variables
    http_server_->Get("/webapp.html", [this, static_dir](const httplib::Request&, httplib::Response& res) {
        std::string webapp_path = static_dir + "/webapp.html";
        std::ifstream file(webapp_path);
        
        if (!file.is_open()) {
            std::cerr << "[Server] Could not open webapp.html at: " << webapp_path << std::endl;
            res.status = 404;
            res.set_content("{\"error\": \"webapp.html not found\"}", "application/json");
            return;
        }
        
        // Read the entire file
        std::string html_template((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        
        // Get filtered models from model manager
        auto models_map = model_manager_->get_supported_models();
        
        // Convert map to JSON
        json filtered_models = json::object();
        for (const auto& [model_name, info] : models_map) {
            filtered_models[model_name] = {
                {"model_name", info.model_name},
                {"checkpoint", info.checkpoint},
                {"recipe", info.recipe},
                {"labels", info.labels},
                {"suggested", info.suggested},
                {"mmproj", info.mmproj}
            };
        }
        
        // Create JavaScript snippets
        std::string server_models_js = "<script>window.SERVER_MODELS = " + filtered_models.dump() + ";</script>";
        
        // Get platform name
        std::string platform_name;
        #ifdef _WIN32
            platform_name = "Windows";
        #elif __APPLE__
            platform_name = "Darwin";
        #elif __linux__
            platform_name = "Linux";
        #else
            platform_name = "Unknown";
        #endif
        std::string platform_js = "<script>window.PLATFORM = '" + platform_name + "';</script>";
        
        // Replace template variables
        size_t pos;
        
        // Replace {{SERVER_PORT}}
        while ((pos = html_template.find("{{SERVER_PORT}}")) != std::string::npos) {
            html_template.replace(pos, 17, std::to_string(port_));
        }
        
        // Replace {{SERVER_MODELS_JS}}
        while ((pos = html_template.find("{{SERVER_MODELS_JS}}")) != std::string::npos) {
            html_template.replace(pos, 20, server_models_js);
        }
        
        // Replace {{PLATFORM_JS}}
        while ((pos = html_template.find("{{PLATFORM_JS}}")) != std::string::npos) {
            html_template.replace(pos, 15, platform_js);
        }
        
        // Set no-cache headers
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        res.set_content(html_template, "text/html");
    });
    
    // Mount static files directory for other files (CSS, JS, images)
    // Use /static prefix to avoid conflicts with webapp.html
    if (!http_server_->set_mount_point("/static", static_dir)) {
        std::cerr << "[Server WARNING] Could not mount static files from: " << static_dir << std::endl;
        std::cerr << "[Server] Web UI assets will not be available" << std::endl;
    } else {
        std::cout << "[Server] Static files mounted from: " << static_dir << std::endl;
    }
    
    // Override default headers for static files to include no-cache
    // This ensures the web UI always gets the latest version
    http_server_->set_file_request_handler([](const httplib::Request& req, httplib::Response& res) {
        // Add no-cache headers for static files
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
    });
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
        std::cout << "[Server] Stopping HTTP server..." << std::endl;
        http_server_->stop();
        running_ = false;
        
        // Explicitly clean up router (unload models, stop backend servers)
        if (router_) {
            std::cout << "[Server] Unloading models and stopping backend servers..." << std::endl;
            try {
                router_->unload_model();
            } catch (const std::exception& e) {
                std::cerr << "[Server] Error during cleanup: " << e.what() << std::endl;
            }
        }
        std::cout << "[Server] Cleanup complete" << std::endl;
    }
}

bool Server::is_running() const {
    return running_;
}

// Helper function for auto-loading models on inference and load endpoints
// ========================================================================
// This function is called by:
//   - handle_chat_completions() - /chat/completions endpoint
//   - handle_completions() - /completions endpoint  
//   - handle_load() - /load endpoint
//
// Behavior:
//   1. If model is already loaded: Return immediately (no-op)
//   2. If model is not downloaded: Download it (first-time use)
//   3. If model is downloaded: Use cached version (don't check HuggingFace for updates)
//
// Note: Only the /pull endpoint checks HuggingFace for updates (do_not_upgrade=false)
void Server::auto_load_model_if_needed(const std::string& requested_model) {
    std::string loaded_model = router_->get_loaded_model();
    
    // Early return if already loaded
    if (loaded_model == requested_model) {
        std::cout << "[Server] Model already loaded: " << requested_model << std::endl;
        return;
    }
    
    // Log the auto-loading action
    if (!loaded_model.empty()) {
        std::cout << "[Server] Switching from '" << loaded_model << "' to '" << requested_model << "'" << std::endl;
    } else {
        std::cout << "[Server] No model loaded, auto-loading: " << requested_model << std::endl;
    }
    
    // Get model info
    if (!model_manager_->model_exists(requested_model)) {
        throw std::runtime_error("Model not found: " + requested_model);
    }
    
    auto info = model_manager_->get_model_info(requested_model);
    
    // Download model if not cached (first-time use)
    // IMPORTANT: Use do_not_upgrade=true to prevent checking HuggingFace for updates
    // This means:
    //   - If model is NOT downloaded: Download it from HuggingFace
    //   - If model IS downloaded: Skip HuggingFace API check entirely (use cached version)
    // Only the /pull endpoint should check for updates (uses do_not_upgrade=false)
    if (info.recipe != "flm" && !model_manager_->is_model_downloaded(requested_model)) {
        std::cout << "[Server] Model not cached, downloading from Hugging Face..." << std::endl;
        std::cout << "[Server] This may take several minutes for large models." << std::endl;
        model_manager_->download_model(requested_model, "", "", false, false, "", true);
        std::cout << "[Server] Model download complete: " << requested_model << std::endl;
        
        // CRITICAL: Refresh model info after download to get correct resolved_path
        // The resolved_path is computed based on filesystem, so we need fresh info now that files exist
        info = model_manager_->get_model_info(requested_model);
    }
    
    // Load model with do_not_upgrade=true
    // For FLM models: FastFlowLMServer will handle download internally if needed
    // For non-FLM models: Model should already be cached at this point
    router_->load_model(requested_model, info, true);
    std::cout << "[Server] Model loaded successfully: " << requested_model << std::endl;
}

void Server::handle_health(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }
    
    auto thread_id = std::this_thread::get_id();
    std::cout << "[Server DEBUG] ===== HEALTH ENDPOINT ENTERED (Thread: " << thread_id << ") =====" << std::endl;
    std::cout.flush();
    
    nlohmann::json response = {{"status", "ok"}};
    
    // Add model loaded information like Python implementation
    std::string loaded_checkpoint = router_->get_loaded_checkpoint();
    std::string loaded_model = router_->get_loaded_model();
    
    response["checkpoint_loaded"] = loaded_checkpoint.empty() ? nlohmann::json(nullptr) : nlohmann::json(loaded_checkpoint);
    response["model_loaded"] = loaded_model.empty() ? nlohmann::json(nullptr) : nlohmann::json(loaded_model);
    
    // Add context size
    response["context_size"] = router_->get_ctx_size();
    
    // Add log streaming support information
    response["log_streaming"] = {
        {"sse", true},
        {"websocket", false}  // WebSocket support not yet implemented
    };
    
    res.set_content(response.dump(), "application/json");
    std::cout << "[Server DEBUG] ===== HEALTH ENDPOINT RETURNING (Thread: " << thread_id << ") =====" << std::endl;
    std::cout.flush();
}

void Server::handle_models(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }
    
    std::cout << "[Server DEBUG] ===== MODELS ENDPOINT ENTERED =====" << std::endl;
    std::cout.flush();
    
    // Check if we should show all models (for CLI list command) or only downloaded (OpenAI API behavior)
    bool show_all = req.has_param("show_all") && req.get_param_value("show_all") == "true";
    
    // OPTIMIZATION: For OpenAI API mode, use get_downloaded_models() which filters first
    // Only use get_supported_models() when we need to show ALL models
    std::map<std::string, ModelInfo> models;
    if (show_all) {
        models = model_manager_->get_supported_models();
    } else {
        models = model_manager_->get_downloaded_models();
    }
    
    nlohmann::json response;
    response["data"] = nlohmann::json::array();
    response["object"] = "list";
    
    for (const auto& [model_id, model_info] : models) {
        
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
            // Need to check download status for each model when showing all
            bool is_downloaded = model_manager_->is_model_downloaded(model_id);
            
            model_json["name"] = model_info.model_name;
            model_json["downloaded"] = is_downloaded;
            model_json["labels"] = model_info.labels;
        }
        
        response["data"].push_back(model_json);
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
        
        // Debug: Check if tools are present
        if (request_json.contains("tools")) {
            std::cout << "[Server DEBUG] Tools present in request: " << request_json["tools"].size() << " tool(s)" << std::endl;
            std::cout << "[Server DEBUG] Tools JSON: " << request_json["tools"].dump() << std::endl;
        } else {
            std::cout << "[Server DEBUG] No tools in request" << std::endl;
        }
        
        // Handle model loading/switching
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load model: " << e.what() << std::endl;
                res.status = 404;
                res.set_content("{\"error\": \"" + std::string(e.what()) + "\"}", "application/json");
                return;
            }
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();
        
        // FLM requires the checkpoint name in the request, not the Lemonade model name
        std::string request_body = req.body;
        if (router_->get_loaded_recipe() == "flm") {
            auto request_json_copy = request_json;
            request_json_copy["model"] = router_->get_loaded_checkpoint();
            request_body = request_json_copy.dump();
            if (!is_streaming) {
                request_json = request_json_copy;  // Update for non-streaming path too
            }
        }
        
        if (is_streaming) {
            try {
                // Log the HTTP request
                std::cout << "[Server] POST /api/v1/chat/completions - Streaming" << std::endl;
                
                // Set up streaming response with SSE headers
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                res.set_header("X-Accel-Buffering", "no"); // Disable nginx buffering
                
                // Use cpp-httplib's chunked content provider for SSE streaming
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this, request_body](size_t offset, httplib::DataSink& sink) {
                        // For chunked responses, offset tracks bytes sent so far
                        // We only want to stream once when offset is 0
                        if (offset > 0) {
                            return false; // We're done after the first call
                        }
                        
                        // Use unified Router path for streaming
                        router_->chat_completion_stream(request_body, sink);
                        
                        // Return false to indicate we're done streaming
                        return false;
                    }
                );
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\":\"Internal server error during streaming\"}", "application/json");
            }
        } else {
            // Log the HTTP request
            std::cout << "[Server] POST /api/v1/chat/completions - ";
            
            auto response = router_->chat_completion(request_json);
            
            // Complete the log line with status
            std::cout << "200 OK" << std::endl;
            
            // Debug: Check if response contains tool_calls
            if (response.contains("choices") && response["choices"].is_array() && !response["choices"].empty()) {
                auto& first_choice = response["choices"][0];
                if (first_choice.contains("message")) {
                    auto& message = first_choice["message"];
                    if (message.contains("tool_calls")) {
                        std::cout << "[Server DEBUG] Response contains tool_calls: " << message["tool_calls"].dump() << std::endl;
                    } else {
                        std::cout << "[Server DEBUG] Response message does NOT contain tool_calls" << std::endl;
                        if (message.contains("content")) {
                            std::cout << "[Server DEBUG] Message content: " << message["content"].get<std::string>().substr(0, 200) << std::endl;
                        }
                    }
                }
            }
            
            res.set_content(response.dump(), "application/json");
            
            // Print and save telemetry for non-streaming
            // llama-server includes timing data in the response under "timings" field
            if (response.contains("timings")) {
                auto timings = response["timings"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;
                
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (timings.contains("prompt_n")) {
                    input_tokens = timings["prompt_n"].get<int>();
                    std::cout << "Input tokens:  " << input_tokens << std::endl;
                }
                if (timings.contains("predicted_n")) {
                    output_tokens = timings["predicted_n"].get<int>();
                    std::cout << "Output tokens: " << output_tokens << std::endl;
                }
                if (timings.contains("prompt_ms")) {
                    ttft_seconds = timings["prompt_ms"].get<double>() / 1000.0;
                    std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                             << ttft_seconds << std::endl;
                }
                if (timings.contains("predicted_per_second")) {
                    tps = timings["predicted_per_second"].get<double>();
                    std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                             << tps << std::endl;
                }
                std::cout << "=================" << std::endl;
                
                // Save telemetry to router
                router_->update_telemetry(input_tokens, output_tokens, ttft_seconds, tps);
            } else if (response.contains("usage")) {
                // OpenAI format uses "usage" field
                auto usage = response["usage"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;
                
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (usage.contains("prompt_tokens")) {
                    input_tokens = usage["prompt_tokens"].get<int>();
                    std::cout << "Input tokens:  " << input_tokens << std::endl;
                }
                if (usage.contains("completion_tokens")) {
                    output_tokens = usage["completion_tokens"].get<int>();
                    std::cout << "Output tokens: " << output_tokens << std::endl;
                }
                
                // FLM format may include timing data
                if (usage.contains("prefill_duration_ttft")) {
                    ttft_seconds = usage["prefill_duration_ttft"].get<double>();
                    std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                             << ttft_seconds << std::endl;
                }
                if (usage.contains("decoding_speed_tps")) {
                    tps = usage["decoding_speed_tps"].get<double>();
                    std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                             << tps << std::endl;
                }
                std::cout << "=================" << std::endl;
                
                // Save telemetry to router
                router_->update_telemetry(input_tokens, output_tokens, ttft_seconds, tps);
            }
            
            // Capture prompt_tokens from usage if available
            if (response.contains("usage")) {
                auto usage = response["usage"];
                if (usage.contains("prompt_tokens")) {
                    int prompt_tokens = usage["prompt_tokens"].get<int>();
                    router_->update_prompt_tokens(prompt_tokens);
                }
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
        
        // Handle model loading/switching (same logic as chat_completions)
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load model: " << e.what() << std::endl;
                res.status = 404;
                res.set_content("{\"error\": \"" + std::string(e.what()) + "\"}", "application/json");
                return;
            }
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();
        
        // FLM requires the checkpoint name in the request, not the Lemonade model name
        std::string request_body = req.body;
        if (router_->get_loaded_recipe() == "flm") {
            auto request_json_copy = request_json;
            request_json_copy["model"] = router_->get_loaded_checkpoint();
            request_body = request_json_copy.dump();
            if (!is_streaming) {
                request_json = request_json_copy;  // Update for non-streaming path too
            }
        }
        
        if (is_streaming) {
            try {
                // Log the HTTP request
                std::cout << "[Server] POST /api/v1/completions - Streaming" << std::endl;
                
                // Set up SSE headers
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                res.set_header("X-Accel-Buffering", "no");
                
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this, request_body](size_t offset, httplib::DataSink& sink) {
                        if (offset > 0) {
                            return false; // Already sent everything
                        }
                        
                        // Use unified Router path for streaming
                        router_->completion_stream(request_body, sink);
                        
                        return false; // Signal completion
                    }
                );
                
                std::cout << "[Server] Streaming completed - 200 OK" << std::endl;
                return;
                
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\": \"" + std::string(e.what()) + "\"}", "application/json");
                return;
            }
        } else {
            // Non-streaming
            auto response = router_->completion(request_json);
            
            // Check if response contains an error
            if (response.contains("error")) {
                std::cerr << "[Server] ERROR: Backend returned error response: " << response["error"].dump() << std::endl;
                res.status = 500;
                res.set_content(response.dump(), "application/json");
                return;
            }
            
            // Verify response has required fields
            if (!response.contains("choices")) {
                std::cerr << "[Server] ERROR: Response missing 'choices' field. Response: " << response.dump() << std::endl;
                res.status = 500;
                nlohmann::json error = {{"error", "Backend returned invalid response format"}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            
            res.set_content(response.dump(), "application/json");
            
            // Print and save telemetry for non-streaming completions
            if (response.contains("timings")) {
                auto timings = response["timings"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;
                
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (timings.contains("prompt_n")) {
                    input_tokens = timings["prompt_n"].get<int>();
                    std::cout << "Input tokens:  " << input_tokens << std::endl;
                }
                if (timings.contains("predicted_n")) {
                    output_tokens = timings["predicted_n"].get<int>();
                    std::cout << "Output tokens: " << output_tokens << std::endl;
                }
                if (timings.contains("prompt_ms")) {
                    ttft_seconds = timings["prompt_ms"].get<double>() / 1000.0;
                    std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                             << ttft_seconds << std::endl;
                }
                if (timings.contains("predicted_per_second")) {
                    tps = timings["predicted_per_second"].get<double>();
                    std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                             << tps << std::endl;
                }
                std::cout << "=================" << std::endl;
                
                // Save telemetry to router
                router_->update_telemetry(input_tokens, output_tokens, ttft_seconds, tps);
            } else if (response.contains("usage")) {
                auto usage = response["usage"];
                int input_tokens = 0;
                int output_tokens = 0;
                double ttft_seconds = 0.0;
                double tps = 0.0;
                
                std::cout << "\n=== Telemetry ===" << std::endl;
                if (usage.contains("prompt_tokens")) {
                    input_tokens = usage["prompt_tokens"].get<int>();
                    std::cout << "Input tokens:  " << input_tokens << std::endl;
                }
                if (usage.contains("completion_tokens")) {
                    output_tokens = usage["completion_tokens"].get<int>();
                    std::cout << "Output tokens: " << output_tokens << std::endl;
                }
                
                // FLM format may include timing data
                if (usage.contains("prefill_duration_ttft")) {
                    ttft_seconds = usage["prefill_duration_ttft"].get<double>();
                    std::cout << "TTFT (s):      " << std::fixed << std::setprecision(2) 
                             << ttft_seconds << std::endl;
                }
                if (usage.contains("decoding_speed_tps")) {
                    tps = usage["decoding_speed_tps"].get<double>();
                    std::cout << "TPS:           " << std::fixed << std::setprecision(2) 
                             << tps << std::endl;
                }
                std::cout << "=================" << std::endl;
                
                // Save telemetry to router
                router_->update_telemetry(input_tokens, output_tokens, ttft_seconds, tps);
            }
            
            // Capture prompt_tokens from usage if available
            if (response.contains("usage")) {
                auto usage = response["usage"];
                if (usage.contains("prompt_tokens")) {
                    int prompt_tokens = usage["prompt_tokens"].get<int>();
                    router_->update_prompt_tokens(prompt_tokens);
                }
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_completions: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_embeddings(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        
        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            auto_load_model_if_needed(requested_model);
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Call router's embeddings method
        auto response = router_->embeddings(request_json);
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_embeddings: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_reranking(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        
        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            auto_load_model_if_needed(requested_model);
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Call router's reranking method
        auto response = router_->reranking(request_json);
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_reranking: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_responses(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        
        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            auto_load_model_if_needed(requested_model);
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }
        
        // Check if current model supports responses API (only oga-* recipes)
        std::string loaded_recipe = router_->get_loaded_recipe();
        if (loaded_recipe.find("oga-") == std::string::npos && loaded_recipe != "oga") {
            std::cerr << "[Server ERROR] Responses API not supported for recipe: " << loaded_recipe << std::endl;
            res.status = 422;
            nlohmann::json error_response = {
                {"error", {
                    {"message", "Responses API not supported for recipe: " + loaded_recipe},
                    {"type", "unsupported_recipe"},
                    {"code", "responses_not_supported"}
                }}
            };
            res.set_content(error_response.dump(), "application/json");
            return;
        }
        
        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();
        
        if (is_streaming) {
            try {
                std::cout << "[Server] POST /api/v1/responses - Streaming" << std::endl;
                
                // Set up streaming response with SSE headers
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");
                res.set_header("X-Accel-Buffering", "no");
                
                // Use cpp-httplib's chunked content provider for SSE streaming
                res.set_chunked_content_provider(
                    "text/event-stream",
                    [this, request_body = req.body](size_t offset, httplib::DataSink& sink) {
                        if (offset > 0) {
                            return false; // Only stream once
                        }
                        
                        // Use unified Router path for streaming
                        router_->responses_stream(request_body, sink);
                        
                        return false;
                    }
                );
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Streaming failed: " << e.what() << std::endl;
                res.status = 500;
                res.set_content("{\"error\":\"Internal server error during streaming\"}", "application/json");
            }
        } else {
            std::cout << "[Server] POST /api/v1/responses - Non-streaming" << std::endl;
            
            auto response = router_->responses(request_json);
            
            std::cout << "200 OK" << std::endl;
            res.set_content(response.dump(), "application/json");
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_responses: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_pull(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        // Accept both "model" and "model_name" for compatibility
        std::string model_name = request_json.contains("model") ? 
            request_json["model"].get<std::string>() : 
            request_json["model_name"].get<std::string>();
        
        // Extract optional parameters
        std::string checkpoint = request_json.value("checkpoint", "");
        std::string recipe = request_json.value("recipe", "");
        bool reasoning = request_json.value("reasoning", false);
        bool vision = request_json.value("vision", false);
        std::string mmproj = request_json.value("mmproj", "");
        bool do_not_upgrade = request_json.value("do_not_upgrade", false);
        
        std::cout << "[Server] Pulling model: " << model_name << std::endl;
        if (!checkpoint.empty()) {
            std::cout << "[Server]   checkpoint: " << checkpoint << std::endl;
        }
        if (!recipe.empty()) {
            std::cout << "[Server]   recipe: " << recipe << std::endl;
        }
        
        model_manager_->download_model(model_name, checkpoint, recipe, 
                                      reasoning, vision, mmproj, do_not_upgrade);
        
        nlohmann::json response = {{"status", "success"}, {"model_name", model_name}};
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_pull: " << e.what() << std::endl;
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
        
        // Check if model is already loaded (early return optimization)
        std::string loaded_model = router_->get_loaded_model();
        if (loaded_model == model_name) {
            std::cout << "[Server] Model already loaded: " << model_name << std::endl;
            auto info = model_manager_->get_model_info(model_name);
            nlohmann::json response = {
                {"status", "success"},
                {"model_name", model_name},
                {"checkpoint", info.checkpoint},
                {"recipe", info.recipe},
                {"message", "Model already loaded"}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }
        
        // Use helper function to handle loading (download outside mutex, no race conditions)
        auto_load_model_if_needed(model_name);
        
        // Get model info for response
        auto info = model_manager_->get_model_info(model_name);
        nlohmann::json response = {
            {"status", "success"},
            {"model_name", model_name},
            {"checkpoint", info.checkpoint},
            {"recipe", info.recipe}
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
        // Accept both "model" and "model_name" for compatibility
        std::string model_name = request_json.contains("model") ? 
            request_json["model"].get<std::string>() : 
            request_json["model_name"].get<std::string>();
        
        std::cout << "[Server] Deleting model: " << model_name << std::endl;
        model_manager_->delete_model(model_name);
        
        nlohmann::json response = {
            {"status", "success"}, 
            {"message", "Deleted model: " + model_name}
        };
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_delete: " << e.what() << std::endl;
        
        // Check if this is a "Model not found" error (return 422)
        std::string error_msg = e.what();
        if (error_msg.find("Model not found") != std::string::npos ||
            error_msg.find("not supported") != std::string::npos) {
            res.status = 422;
        } else {
            res.status = 500;
        }
        
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
        std::cerr << "[Server] ERROR in handle_params: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_add_local_model(const httplib::Request& req, httplib::Response& res) {
    try {
        std::cout << "[Server] Add local model request received" << std::endl;
        
        // Validate that this is a multipart form request
        if (!req.is_multipart_form_data()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Request must be multipart/form-data"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        
        // Extract form fields
        std::string model_name;
        std::string checkpoint;
        std::string recipe;
        std::string mmproj;
        bool reasoning = false;
        bool vision = false;
        
        // Parse form fields
        if (req.form.has_field("model_name")) {
            model_name = req.form.get_field("model_name");
        }
        if (req.form.has_field("checkpoint")) {
            checkpoint = req.form.get_field("checkpoint");
        }
        if (req.form.has_field("recipe")) {
            recipe = req.form.get_field("recipe");
        }
        if (req.form.has_field("mmproj")) {
            mmproj = req.form.get_field("mmproj");
        }
        if (req.form.has_field("reasoning")) {
            std::string reasoning_str = req.form.get_field("reasoning");
            reasoning = (reasoning_str == "true" || reasoning_str == "True" || reasoning_str == "1");
        }
        if (req.form.has_field("vision")) {
            std::string vision_str = req.form.get_field("vision");
            vision = (vision_str == "true" || vision_str == "True" || vision_str == "1");
        }
        
        std::cout << "[Server] Model name: " << model_name << std::endl;
        std::cout << "[Server] Recipe: " << recipe << std::endl;
        std::cout << "[Server] Checkpoint: " << checkpoint << std::endl;
        
        // Validate required fields
        if (model_name.empty() || recipe.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", "model_name and recipe are required"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        
        // Validate model name starts with "user."
        if (model_name.substr(0, 5) != "user.") {
            res.status = 400;
            nlohmann::json error = {{"error", "Model name must start with 'user.'"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        
        // Validate recipe
        std::vector<std::string> valid_recipes = {"llamacpp", "oga-npu", "oga-hybrid", "oga-cpu"};
        if (std::find(valid_recipes.begin(), valid_recipes.end(), recipe) == valid_recipes.end()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Invalid recipe. Must be one of: llamacpp, oga-npu, oga-hybrid, oga-cpu"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        
        // Check if model files are provided
        const auto& files = req.form.files;
        if (files.empty()) {
            res.status = 400;
            nlohmann::json error = {{"error", "No model files provided for upload"}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        
        // For llamacpp, ensure at least one .gguf file is present
        if (recipe == "llamacpp") {
            bool has_gguf = false;
            for (const auto& file_pair : files) {
                std::string filename = file_pair.second.filename;
                std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
                if (filename.find(".gguf") != std::string::npos) {
                    has_gguf = true;
                    break;
                }
            }
            if (!has_gguf) {
                res.status = 400;
                nlohmann::json error = {{"error", "At least one .gguf file is required for llamacpp"}};
                res.set_content(error.dump(), "application/json");
                return;
            }
        }
        
        // Check if model name already exists
        if (model_manager_->model_exists(model_name)) {
            res.status = 409;
            nlohmann::json error = {{"error", "Model name '" + model_name + "' already exists. Please use a different name."}};
            res.set_content(error.dump(), "application/json");
            return;
        }
        
        // Get HF cache directory
        std::string hf_cache;
        const char* hf_home_env = std::getenv("HF_HOME");
        if (hf_home_env) {
            hf_cache = std::string(hf_home_env) + "/hub";
        } else {
#ifdef _WIN32
            const char* userprofile = std::getenv("USERPROFILE");
            if (userprofile) {
                hf_cache = std::string(userprofile) + "\\.cache\\huggingface\\hub";
            } else {
                throw std::runtime_error("Cannot determine HF cache directory");
            }
#else
            const char* home = std::getenv("HOME");
            if (home) {
                hf_cache = std::string(home) + "/.cache/huggingface/hub";
            } else {
                throw std::runtime_error("Cannot determine HF cache directory");
            }
#endif
        }
        
        // Create model directory in HF cache
        std::string model_name_clean = model_name.substr(5); // Remove "user." prefix
        std::string repo_cache_name = model_name_clean;
        // Replace / with --
        std::replace(repo_cache_name.begin(), repo_cache_name.end(), '/', '-');
        
        std::string snapshot_path = hf_cache + "/models--" + repo_cache_name;
        std::cout << "[Server] Creating directory: " << snapshot_path << std::endl;
        
        // Create directories
        std::filesystem::create_directories(snapshot_path);
        
        // Extract variant from checkpoint field if provided
        std::string variant;
        if (!checkpoint.empty() && checkpoint.find(':') != std::string::npos) {
            size_t colon_pos = checkpoint.find(':');
            variant = checkpoint.substr(colon_pos + 1);
        }
        
        // Save uploaded files
        std::cout << "[Server] Saving " << files.size() << " uploaded files..." << std::endl;
        for (const auto& file_pair : files) {
            // Skip form fields (model_name, recipe, etc.)
            if (file_pair.first != "model_files") {
                continue;
            }
            
            const auto& file = file_pair.second;
            std::string filename = file.filename;
            std::cout << "[Server]   Processing file: " << filename << std::endl;
            
            // Extract relative path from filename (browser sends folder/file.ext)
            std::string file_path;
            size_t first_slash = filename.find('/');
            if (first_slash != std::string::npos) {
                // Has folder structure - use everything after first slash
                file_path = snapshot_path + "/" + filename.substr(first_slash + 1);
            } else {
                // No folder structure - save directly
                file_path = snapshot_path + "/" + filename;
            }
            
            // Create parent directories
            std::filesystem::path parent_dir = std::filesystem::path(file_path).parent_path();
            std::filesystem::create_directories(parent_dir);
            
            // Write file
            std::ofstream out(file_path, std::ios::binary);
            if (!out) {
                throw std::runtime_error("Failed to create file: " + file_path);
            }
            out.write(file.content.c_str(), file.content.size());
            out.close();
            
            std::cout << "[Server]     Saved to: " << file_path << std::endl;
        }
        
        // Resolve actual file paths after upload
        std::string resolved_checkpoint;
        std::string resolved_mmproj;
        
        // For OGA models, find genai_config.json
        if (recipe.find("oga-") == 0) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(snapshot_path)) {
                if (entry.is_regular_file() && entry.path().filename() == "genai_config.json") {
                    resolved_checkpoint = entry.path().parent_path().string();
                    break;
                }
            }
            if (resolved_checkpoint.empty()) {
                resolved_checkpoint = snapshot_path;
            }
        }
        // For llamacpp models, find the GGUF file
        else if (recipe == "llamacpp") {
            std::string gguf_file_found;
            
            // If variant is specified, look for that specific file
            if (!variant.empty()) {
                std::string search_term = variant;
                if (variant.find(".gguf") == std::string::npos) {
                    search_term = variant + ".gguf";
                }
                
                for (const auto& entry : std::filesystem::recursive_directory_iterator(snapshot_path)) {
                    if (entry.is_regular_file() && entry.path().filename() == search_term) {
                        gguf_file_found = entry.path().string();
                        break;
                    }
                }
            }
            
            // If no variant or variant not found, search for any .gguf file (excluding mmproj)
            if (gguf_file_found.empty()) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(snapshot_path)) {
                    if (entry.is_regular_file()) {
                        std::string filename = entry.path().filename().string();
                        std::string filename_lower = filename;
                        std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);
                        
                        if (filename_lower.find(".gguf") != std::string::npos && 
                            filename_lower.find("mmproj") == std::string::npos) {
                            gguf_file_found = entry.path().string();
                            break;
                        }
                    }
                }
            }
            
            resolved_checkpoint = gguf_file_found.empty() ? snapshot_path : gguf_file_found;
        }
        
        // Search for mmproj file if provided
        if (!mmproj.empty()) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(snapshot_path)) {
                if (entry.is_regular_file() && entry.path().filename() == mmproj) {
                    resolved_mmproj = entry.path().string();
                    break;
                }
            }
        }
        
        // Build checkpoint for registration - store as relative path from HF cache
        std::string checkpoint_to_register;
        if (!resolved_checkpoint.empty()) {
            std::filesystem::path rel = std::filesystem::relative(resolved_checkpoint, hf_cache);
            checkpoint_to_register = rel.string();
        } else {
            // Fallback if no files found - use directory path
            checkpoint_to_register = "models--" + repo_cache_name;
        }
        
        std::cout << "[Server] Registering model with checkpoint: " << checkpoint_to_register << std::endl;
        
        // Register the model with source="local_upload" to mark it as locally uploaded
        model_manager_->register_user_model(
            model_name,
            checkpoint_to_register,
            recipe,
            reasoning,
            vision,
            resolved_mmproj.empty() ? mmproj : resolved_mmproj,
            "local_upload"
        );
        
        std::cout << "[Server] Model registered successfully" << std::endl;
        
        nlohmann::json response = {
            {"status", "success"},
            {"message", "Model " + model_name + " uploaded and registered successfully"}
        };
        res.set_content(response.dump(), "application/json");
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_add_local_model: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", "Failed to upload model: " + std::string(e.what())}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_stats(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }
    
    try {
        auto stats = router_->get_stats();
        res.set_content(stats.dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_stats: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_system_info(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }
    
    try {
        // Get verbose parameter from query string (default to false)
        bool verbose = false;
        if (req.has_param("verbose")) {
            std::string verbose_param = req.get_param_value("verbose");
            std::transform(verbose_param.begin(), verbose_param.end(), verbose_param.begin(), ::tolower);
            verbose = (verbose_param == "true" || verbose_param == "1");
        }
        
        // Check cache first
        SystemInfoCache cache;
        nlohmann::json cached_hardware = cache.load_hardware_info();
        nlohmann::json devices;
        
        // Create platform-specific system info instance
        auto sys_info = create_system_info();
        
        if (!cached_hardware.empty()) {
            std::cout << "[Server] Using cached hardware info from: " << cache.get_cache_file_path() << std::endl;
            devices = cached_hardware;
        } else {
            std::cout << "[Server] Detecting hardware (will cache to: " << cache.get_cache_file_path() << ")" << std::endl;
            
            // Get hardware information
            devices = sys_info->get_device_dict();
            
            // Strip inference_engines before caching (hardware only)
            nlohmann::json hardware_only = devices;
            if (hardware_only.contains("cpu") && hardware_only["cpu"].contains("inference_engines")) {
                hardware_only["cpu"].erase("inference_engines");
            }
            if (hardware_only.contains("amd_igpu") && hardware_only["amd_igpu"].contains("inference_engines")) {
                hardware_only["amd_igpu"].erase("inference_engines");
            }
            if (hardware_only.contains("amd_dgpu") && hardware_only["amd_dgpu"].is_array()) {
                for (auto& gpu : hardware_only["amd_dgpu"]) {
                    if (gpu.contains("inference_engines")) {
                        gpu.erase("inference_engines");
                    }
                }
            }
            if (hardware_only.contains("nvidia_dgpu") && hardware_only["nvidia_dgpu"].is_array()) {
                for (auto& gpu : hardware_only["nvidia_dgpu"]) {
                    if (gpu.contains("inference_engines")) {
                        gpu.erase("inference_engines");
                    }
                }
            }
            if (hardware_only.contains("npu") && hardware_only["npu"].contains("inference_engines")) {
                hardware_only["npu"].erase("inference_engines");
            }
            
            // Save hardware-only info to cache
            cache.save_hardware_info(hardware_only);
            std::cout << "[Server] Hardware info cached to: " << cache.get_cache_file_path() << std::endl;
        }
        
        // Detect inference engines (always fresh, never cached)
        // CPU
        if (devices.contains("cpu") && devices["cpu"].contains("name")) {
            std::string cpu_name = devices["cpu"]["name"];
            devices["cpu"]["inference_engines"] = sys_info->detect_inference_engines("cpu", cpu_name);
        }
        
        // AMD iGPU
        if (devices.contains("amd_igpu") && devices["amd_igpu"].contains("name")) {
            std::string gpu_name = devices["amd_igpu"]["name"];
            devices["amd_igpu"]["inference_engines"] = sys_info->detect_inference_engines("amd_igpu", gpu_name);
        }
        
        // AMD dGPUs
        if (devices.contains("amd_dgpu") && devices["amd_dgpu"].is_array()) {
            for (auto& gpu : devices["amd_dgpu"]) {
                if (gpu.contains("name") && !gpu["name"].get<std::string>().empty()) {
                    std::string gpu_name = gpu["name"];
                    gpu["inference_engines"] = sys_info->detect_inference_engines("amd_dgpu", gpu_name);
                }
            }
        }
        
        // NVIDIA dGPUs
        if (devices.contains("nvidia_dgpu") && devices["nvidia_dgpu"].is_array()) {
            for (auto& gpu : devices["nvidia_dgpu"]) {
                if (gpu.contains("name") && !gpu["name"].get<std::string>().empty()) {
                    std::string gpu_name = gpu["name"];
                    gpu["inference_engines"] = sys_info->detect_inference_engines("nvidia_dgpu", gpu_name);
                }
            }
        }
        
        // NPU
        if (devices.contains("npu") && devices["npu"].contains("name")) {
            std::string npu_name = devices["npu"]["name"];
            devices["npu"]["inference_engines"] = sys_info->detect_inference_engines("npu", npu_name);
        }
        
        // Get system information (OS Version, Processor, Physical Memory, etc.)
        nlohmann::json system_info = sys_info->get_system_info_dict();
        
        // Add devices
        system_info["devices"] = devices;
        
        // Filter for non-verbose mode (only essential keys)
        if (!verbose) {
            std::vector<std::string> essential_keys = {"OS Version", "Processor", "Physical Memory", "devices"};
            nlohmann::json filtered_info;
            for (const auto& key : essential_keys) {
                if (system_info.contains(key)) {
                    filtered_info[key] = system_info[key];
                }
            }
            system_info = filtered_info;
        } else {
            // In verbose mode, add Python packages (empty for C++ implementation)
            system_info["Python Packages"] = SystemInfo::get_python_packages();
        }
        
        res.set_content(system_info.dump(), "application/json");
        
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_system_info: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_log_level(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        log_level_ = request_json["level"];
        
        nlohmann::json response = {{"status", "success"}, {"level", log_level_}};
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_log_level: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_shutdown(const httplib::Request& req, httplib::Response& res) {
    std::cout << "[Server] Shutdown request received" << std::endl;
    
    nlohmann::json response = {{"status", "shutting down"}};
    res.set_content(response.dump(), "application/json");
    
    // Stop the server asynchronously to allow response to be sent
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "[Server] Stopping server..." << std::endl;
        std::cout.flush();
        stop();
        
        // Graceful shutdown with timeout: explicitly unload models and stop backend servers
        if (router_) {
            std::cout << "[Server] Unloading models and stopping backend servers..." << std::endl;
            std::cout.flush();
            
            // Just call unload_model directly - keep it simple
            try {
                router_->unload_model();
                std::cout << "[Server] Cleanup completed successfully" << std::endl;
                std::cout.flush();
            } catch (const std::exception& e) {
                std::cerr << "[Server] Error during unload: " << e.what() << std::endl;
                std::cerr.flush();
            }
        }
        
        // Force process exit - just use standard exit()
        std::cout << "[Server] Calling exit(0)..." << std::endl;
        std::cout.flush();
        std::exit(0);
    }).detach();
}

void Server::handle_logs_stream(const httplib::Request& req, httplib::Response& res) {
    // Check if log file exists
    if (log_file_path_.empty() || !std::filesystem::exists(log_file_path_)) {
        std::cerr << "[Server] Log file not found: " << log_file_path_ << std::endl;
        std::cerr << "[Server] Note: Log streaming only works when server is launched via tray/ServerManager" << std::endl;
        res.status = 404;
        nlohmann::json error = {
            {"error", "Log file not found. Log streaming requires server to be launched via tray application."},
            {"path", log_file_path_},
            {"note", "When running directly, logs appear in console instead."}
        };
        res.set_content(error.dump(), "application/json");
        return;
    }
    
    std::cout << "[Server] Starting log stream for: " << log_file_path_ << std::endl;
    
    // Set SSE headers
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");
    
    // Use chunked streaming
    res.set_chunked_content_provider(
        "text/event-stream",
        [this](size_t offset, httplib::DataSink& sink) {
            // Thread-local state for this connection
            static thread_local std::unique_ptr<std::ifstream> log_stream;
            static thread_local std::streampos last_pos = 0;
            
            if (offset == 0) {
                // First call: open file and read from beginning
                log_stream = std::make_unique<std::ifstream>(
                    log_file_path_, 
                    std::ios::in
                );
                
                if (!log_stream->is_open()) {
                    std::cerr << "[Server] Failed to open log file for streaming" << std::endl;
                    return false;
                }
                
                // Start from beginning
                log_stream->seekg(0, std::ios::beg);
                last_pos = 0;
                
                std::cout << "[Server] Log stream connection opened" << std::endl;
            }
            
            // Seek to last known position
            log_stream->seekg(last_pos);
            
            std::string line;
            bool sent_data = false;
            int lines_sent = 0;
            
            // Read and send new lines
            while (std::getline(*log_stream, line)) {
                // Format as SSE: "data: <line>\n\n"
                std::string sse_msg = "data: " + line + "\n\n";
                
                if (!sink.write(sse_msg.c_str(), sse_msg.length())) {
                    std::cout << "[Server] Log stream client disconnected" << std::endl;
                    return false;  // Client disconnected
                }
                
                sent_data = true;
                lines_sent++;
                
                // CRITICAL: Update position after each successful line read
                // Must do this BEFORE hitting EOF, because tellg() returns -1 at EOF!
                last_pos = log_stream->tellg();
            }
            
            // Clear EOF and any other error flags so we can continue reading on next poll
            log_stream->clear();
            
            // Send heartbeat if no data (keeps connection alive)
            if (!sent_data) {
                const char* heartbeat = ": heartbeat\n\n";
                if (!sink.write(heartbeat, strlen(heartbeat))) {
                    std::cout << "[Server] Log stream client disconnected during heartbeat" << std::endl;
                    return false;
                }
            }
            
            // Sleep briefly before next poll
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            return true;  // Keep streaming
        }
    );
}

} // namespace lemon

