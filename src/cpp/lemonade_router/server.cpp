#include "lemon/server.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/streaming_proxy.h"
#include "lemon/system_info.h"
#include "lemon/version.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <memory>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netdb.h>  // Crucial for getaddrinfo and addrinfo struct
    #include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon {

Server::Server(int port, const std::string& host, const std::string& log_level,
               int ctx_size, bool tray, const std::string& llamacpp_backend,
               const std::string& llamacpp_args, int max_llm_models,
               int max_embedding_models, int max_reranking_models, int max_audio_models)
    : port_(port), host_(host), log_level_(log_level), ctx_size_(ctx_size),
      tray_(tray), llamacpp_backend_(llamacpp_backend), llamacpp_args_(llamacpp_args),
      running_(false) {
    
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
    http_server_v6_ = std::make_unique<httplib::Server>();
    
    // CRITICAL: Enable multi-threading so the server can handle concurrent requests
    // Without this, the server is single-threaded and blocks on long operations
     
    std::function<httplib::TaskQueue *(void)> task_queue_factory = [] { 
        std::cout << "[Server DEBUG] Creating new thread pool with 8 threads" << std::endl;
        return new httplib::ThreadPool(8);
    };

    http_server_->new_task_queue = task_queue_factory;
    http_server_v6_->new_task_queue = task_queue_factory;
    
    std::cout << "[Server] HTTP server initialized with thread pool (8 threads)" << std::endl;
    
    model_manager_ = std::make_unique<ModelManager>();
    router_ = std::make_unique<Router>(ctx_size, llamacpp_backend, log_level, llamacpp_args,
                                       model_manager_.get(), max_llm_models,
                                       max_embedding_models, max_reranking_models, max_audio_models);
    
    if (log_level_ == "debug" || log_level_ == "trace") {
        std::cout << "[Server] Debug logging enabled - subprocess output will be visible" << std::endl;
    }
    
    setup_routes(*http_server_);
    setup_routes(*http_server_v6_);
}

Server::~Server() {
    stop();
}

void Server::setup_routes(httplib::Server &web_server) {
    // Add pre-routing handler to log ALL incoming requests
    web_server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        std::cout << "[Server PRE-ROUTE] " << req.method << " " << req.path << std::endl;
        std::cout.flush();
        return httplib::Server::HandlerResponse::Unhandled;
    });
    
    // Setup CORS for all routes
    setup_cors(web_server);
    
    // Helper lambda to register routes for both v0 and v1
    auto register_get = [this, &web_server](const std::string& endpoint, 
                               std::function<void(const httplib::Request&, httplib::Response&)> handler) {
        web_server.Get("/api/v0/" + endpoint, handler);
        web_server.Get("/api/v1/" + endpoint, handler);
    };
    
    auto register_post = [this, &web_server](const std::string& endpoint, 
                                std::function<void(const httplib::Request&, httplib::Response&)> handler) {
        web_server.Post("/api/v0/" + endpoint, handler);
        web_server.Post("/api/v1/" + endpoint, handler);
        // Also register as GET for HEAD request support (HEAD uses GET handler)
        // Return 405 Method Not Allowed (endpoint exists but wrong method)
        web_server.Get("/api/v0/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
            res.status = 405;
            res.set_content("{\"error\": \"Method Not Allowed. Use POST for this endpoint\"}", "application/json");
        });
        web_server.Get("/api/v1/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
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
    web_server.Get(R"(/api/v0/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    web_server.Get(R"(/api/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
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

    // Audio endpoints (OpenAI /v1/audio/* compatible)
    register_post("audio/transcriptions", [this](const httplib::Request& req, httplib::Response& res) {
        handle_audio_transcriptions(req, res);
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
    web_server.Post("/internal/shutdown", [this](const httplib::Request& req, httplib::Response& res) {
        handle_shutdown(req, res);
    });
    
    // Test endpoint to verify POST works
    web_server.Post("/api/v1/test", [](const httplib::Request& req, httplib::Response& res) {
        std::cout << "[Server] TEST POST endpoint hit!" << std::endl;
        res.set_content("{\"test\": \"ok\"}", "application/json");
    });
    
    // Setup static file serving for web UI
    setup_static_files(web_server);
    
    std::cout << "[Server] Routes setup complete" << std::endl;
}

void Server::setup_static_files(httplib::Server &web_server) {
    // Determine static files directory (relative to executable)
    std::string static_dir = utils::get_resource_path("resources/static");
    
    // Create a reusable handler for serving index.html with template variable replacement
    auto serve_index_html = [this, static_dir](const httplib::Request&, httplib::Response& res) {
        std::string index_path = static_dir + "/index.html";
        std::ifstream file(index_path);
        
        if (!file.is_open()) {
            std::cerr << "[Server] Could not open index.html at: " << index_path << std::endl;
            res.status = 404;
            res.set_content("{\"error\": \"index.html not found\"}", "application/json");
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
            
            // Add size if available
            if (info.size > 0.0) {
                filtered_models[model_name]["size"] = info.size;
            }
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
            html_template.replace(pos, 15, std::to_string(port_));
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
    };
    
    // Root path - serve index.html
    web_server.Get("/", serve_index_html);
    
    // Also serve index.html at /api/v1
    web_server.Get("/api/v1", serve_index_html);
    
    // Serve favicon.ico from root as expected by most browsers
    web_server.Get("/favicon.ico", [static_dir](const httplib::Request& req, httplib::Response& res) {
        std::ifstream ifs(static_dir + "/favicon.ico", std::ios::binary);
        if (ifs) {
            // Read favicon bytes to string to pass to response
            std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
            res.set_content(content, "image/x-icon");
            res.status = 200;
        } else {
            res.set_content("Favicon not found.", "text/plain");
            res.status = 404;
        }
    });

    // Mount static files directory for other files (CSS, JS, images)
    if (!web_server.set_mount_point("/static", static_dir)) {
        std::cerr << "[Server WARNING] Could not mount static files from: " << static_dir << std::endl;
        std::cerr << "[Server] Web UI assets will not be available" << std::endl;
    } else {
        std::cout << "[Server] Static files mounted from: " << static_dir << std::endl;
    }
    
    // Override default headers for static files to include no-cache
    // This ensures the web UI always gets the latest version
    web_server.set_file_request_handler([](const httplib::Request& req, httplib::Response& res) {
        // Add no-cache headers for static files
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
    });
}

void Server::setup_cors(httplib::Server &web_server) {
    // Set CORS headers for all responses
    web_server.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"}
    });
    
    // Handle preflight OPTIONS requests
    web_server.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });
    
    // Catch-all error handler - must be last!
    web_server.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
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

std::string Server::resolve_host_to_ip(int ai_family, const std::string& host) {
    struct addrinfo hints = {0};
    hints.ai_family = ai_family; 
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG; // Optional: Only return IPs configured on system

    struct addrinfo *result = nullptr;
    
    // Check return value (0 is success)
    if (httplib::detail::getaddrinfo_with_timeout(host.c_str(), "", &hints, &result, 5000) != 0) {
        std::cerr << "[Server] Warning: resolution failed for " << host << " no " << (ai_family == AF_INET ? "IPv4" : ai_family == AF_INET6 ? "IPv6" : "") << " resolution found." << std::endl;
        return ""; // Return empty string on failure, don't return void
    }

    if (result == nullptr) return "";

    // Use INET6_ADDRSTRLEN to be safe for both (it's larger)
    char addrstr[INET6_ADDRSTRLEN]; 
    void *ptr = nullptr;

    // Safety Check - verify what we actually got back
    if (result->ai_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)result->ai_addr;
        ptr = &(ipv4->sin_addr);
    } else if (result->ai_family == AF_INET6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)result->ai_addr;
        ptr = &(ipv6->sin6_addr);
    } else {
        freeaddrinfo(result);
        return "";
    }

    // Convert binary IP to string
    inet_ntop(result->ai_family, ptr, addrstr, sizeof(addrstr));
    
    std::string resolved_ip(addrstr);
    std::cout << "[Server] Resolved " << host << " (" << (ai_family == AF_INET ? "v4" : "v6") 
              << ") -> " << resolved_ip << std::endl;
              
    freeaddrinfo(result);
    return resolved_ip;
}

void Server::setup_http_logger(httplib::Server &web_server) {
    // Add request logging for ALL requests
    web_server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        std::cout << "[Server] " << req.method << " " << req.path << " - " << res.status << std::endl;
    });
}

void Server::run() {
    std::cout << "[Server] Starting on " << host_ << ":" << port_ << std::endl;
    
    std::string ipv4 = resolve_host_to_ip(AF_INET, host_);
    std::string ipv6 = resolve_host_to_ip(AF_INET6, host_);

    running_ = true;
    if (!ipv4.empty()) {
        // setup ipv4 thread
        setup_http_logger(*http_server_);
        http_v4_thread_ = std::thread([this, ipv4]() {
            http_server_->bind_to_port(ipv4, port_);
            http_server_->listen_after_bind();
        });
    }
    if (!ipv6.empty()) {
        // setup ipv6 thread
        setup_http_logger(*http_server_v6_);
        http_v6_thread_ = std::thread([this, ipv6]() {
            http_server_v6_->bind_to_port(ipv6, port_);
            http_server_v6_->listen_after_bind();
        });
    }
    if(http_v4_thread_.joinable())
        http_v4_thread_.join();
    if(http_v6_thread_.joinable())
        http_v6_thread_.join();
}

void Server::stop() {
    if (running_) {
        std::cout << "[Server] Stopping HTTP server..." << std::endl;
        http_server_v6_->stop();
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
    // Check if this specific model is already loaded (multi-model aware)
    if (router_->is_model_loaded(requested_model)) {
        std::cout << "[Server] Model already loaded: " << requested_model << std::endl;
        return;
    }
    
    // Log the auto-loading action
    std::cout << "[Server] Auto-loading model: " << requested_model << std::endl;
    
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


} // namespace lemon
