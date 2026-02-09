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

#ifdef __APPLE__
    #include <sys/sysctl.h>
#endif

namespace fs = std::filesystem;

namespace lemon {

static const json MIME_TYPES = {
    {"mp3",  "audio/mpeg"},
    {"opus", "audio/opus"},
    {"aac",  "audio/aac"},
    {"flac", "audio/flac"},
    {"wav",  "audio/wav"},
    {"pcm",  "audio/l16;rate=24000;endianness=little-endian"}
};

Server::Server(int port, const std::string& host, const std::string& log_level,
               const json& default_options, int max_llm_models,
               int max_embedding_models, int max_reranking_models, int max_audio_models,
               int max_image_models, const std::string& extra_models_dir, bool no_broadcast)
    : port_(port), host_(host), log_level_(log_level), default_options_(default_options),
      no_broadcast_(no_broadcast), running_(false), udp_beacon_() {

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

    // Set extra models directory for GGUF discovery
    model_manager_->set_extra_models_dir(extra_models_dir);

    router_ = std::make_unique<Router>(default_options_, log_level,
                                       model_manager_.get(), max_llm_models,
                                       max_embedding_models, max_reranking_models, max_audio_models,
                                       max_image_models);

    if (log_level_ == "debug" || log_level_ == "trace") {
        std::cout << "[Server] Debug logging enabled - subprocess output will be visible" << std::endl;
    }

    const char* api_key_env = std::getenv("LEMONADE_API_KEY");
    api_key_ = api_key_env ? std::string(api_key_env) : "";

    setup_routes(*http_server_);
    setup_routes(*http_server_v6_);
}

Server::~Server() {
    stop();
}

void Server::log_request(const httplib::Request& req) {
    if (req.path != "/api/v0/health" && req.path != "/api/v1/health" &&
        req.path != "/v0/health" && req.path != "/v1/health" &&
        req.path != "/api/v0/system-stats" && req.path != "/api/v1/system-stats" &&
        req.path != "/v0/system-stats" && req.path != "/v1/system-stats" &&
        req.path != "/api/v0/stats" && req.path != "/api/v1/stats" &&
        req.path != "/v0/stats" && req.path != "/v1/stats" &&
        req.path != "/live") {
        std::cout << "[Server PRE-ROUTE] " << req.method << " " << req.path << std::endl;
        std::cout.flush();
    }
}

httplib::Server::HandlerResponse Server::authenticate_request(const httplib::Request& req, httplib::Response& res) {
    // Check if path requires authentication (API routes with /api/, /v0/, or /v1/ prefix)
    bool is_api_route = (req.path.rfind("/api/", 0) == 0) ||
                        (req.path.rfind("/v0/", 0) == 0) ||
                        (req.path.rfind("/v1/", 0) == 0);

    if ((api_key_ != "") && (req.method != "OPTIONS") && is_api_route) {
        if (api_key_ != httplib::get_bearer_token_auth(req)) {
            res.status = 401;
            res.set_content("{\"error\": \"Invalid or missing API key\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
    }

    return httplib::Server::HandlerResponse::Unhandled;
}


void Server::setup_routes(httplib::Server &web_server) {
    // Add pre-routing handler to log ALL incoming requests (except health checks)
    web_server.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        this->log_request(req);
        return authenticate_request(req, res);
    });

    web_server.Get("/live", [this](const httplib::Request& req, httplib::Response& res) {
        handle_live(req, res);
    });

    // Setup CORS for all routes
    setup_cors(web_server);

    // Helper lambda to register routes for both v0 and v1 (with and without /api prefix for OpenAI compatibility)
    auto register_get = [this, &web_server](const std::string& endpoint,
                               std::function<void(const httplib::Request&, httplib::Response&)> handler) {
        web_server.Get("/api/v0/" + endpoint, handler);
        web_server.Get("/api/v1/" + endpoint, handler);
        web_server.Get("/v0/" + endpoint, handler);
        web_server.Get("/v1/" + endpoint, handler);
    };

    auto register_post = [this, &web_server](const std::string& endpoint,
                                std::function<void(const httplib::Request&, httplib::Response&)> handler) {
        web_server.Post("/api/v0/" + endpoint, handler);
        web_server.Post("/api/v1/" + endpoint, handler);
        web_server.Post("/v0/" + endpoint, handler);
        web_server.Post("/v1/" + endpoint, handler);
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
        web_server.Get("/v0/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
            res.status = 405;
            res.set_content("{\"error\": \"Method Not Allowed. Use POST for this endpoint\"}", "application/json");
        });
        web_server.Get("/v1/" + endpoint, [](const httplib::Request&, httplib::Response& res) {
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

    // Model by ID (need to register for both versions with regex, with and without /api prefix)
    web_server.Get(R"(/api/v0/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    web_server.Get(R"(/api/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    web_server.Get(R"(/v0/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        handle_model_by_id(req, res);
    });
    web_server.Get(R"(/v1/models/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
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

    // Speech
    register_post("audio/speech", [this](const httplib::Request& req, httplib::Response& res) {
        handle_audio_speech(req, res);
    });

    // Image endpoints (OpenAI /v1/images/* compatible)
    register_post("images/generations", [this](const httplib::Request& req, httplib::Response& res) {
        handle_image_generations(req, res);
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

    // System endpoints
    register_get("stats", [this](const httplib::Request& req, httplib::Response& res) {
        handle_stats(req, res);
    });

    register_get("system-info", [this](const httplib::Request& req, httplib::Response& res) {
        handle_system_info(req, res);
    });

    register_get("system-stats", [this](const httplib::Request& req, httplib::Response& res) {
        handle_system_stats(req, res);
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

    // Keep status page at /status endpoint
    web_server.Get("/status", serve_index_html);

    // Also serve index.html at /api/v1 for compatibility
    web_server.Get("/api/v1", serve_index_html);

    // Mount static files directory for status page assets (CSS, JS, images)
    if (!web_server.set_mount_point("/static", static_dir)) {
        std::cerr << "[Server WARNING] Could not mount static files from: " << static_dir << std::endl;
        std::cerr << "[Server] Status page assets will not be available" << std::endl;
    } else {
        std::cout << "[Server] Static files mounted from: " << static_dir << std::endl;
    }

    // Web app UI endpoint - serve the React web app at root
    std::string web_app_dir = utils::get_resource_path("resources/web-app");

    // Check if web app directory exists
    if (fs::exists(web_app_dir) && fs::is_directory(web_app_dir)) {
        // Create a handler for serving web app index.html for SPA routing
        auto serve_web_app_html = [web_app_dir](const httplib::Request&, httplib::Response& res) {
            std::string index_path = web_app_dir + "/index.html";
            std::ifstream file(index_path);

            if (!file.is_open()) {
                res.status = 404;
                res.set_content("{\"error\": \"Web app not found\"}", "application/json");
                return;
            }

            std::string html((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();

            // Inject mock API for web compatibility with Electron app code
            std::string mock_api = R"(
<script>
// Mock Electron API for web compatibility
window.api = {
    isWebApp: true,  // Explicit flag to indicate web mode
    platform: navigator.platform || 'web',
    minimizeWindow: () => {},
    maximizeWindow: () => {},
    closeWindow: () => {},
    openExternal: (url) => window.open(url, '_blank'),
    onMaximizeChange: () => {},
    updateMinWidth: () => {},
    zoomIn: () => document.body.style.zoom = (parseFloat(document.body.style.zoom || '1') + 0.1).toString(),
    zoomOut: () => document.body.style.zoom = (parseFloat(document.body.style.zoom || '1') - 0.1).toString(),
    getSettings: async () => {
        const saved = localStorage.getItem('lemonade-settings');
        if (saved) return JSON.parse(saved);
        // Return defaults matching DEFAULT_LAYOUT_SETTINGS from appSettings.ts
        return {
            layout: {
                isChatVisible: true,
                isModelManagerVisible: true,
                isCenterPanelVisible: true,
                isLogsVisible: false,
                modelManagerWidth: 280,
                chatWidth: 350,
                logsHeight: 200
            },
            theme: 'dark',
            apiUrl: window.location.origin,
            apiKey: { value: '' }
        };
    },
    saveSettings: async (settings) => {
        localStorage.setItem('lemonade-settings', JSON.stringify(settings));
        return settings;
    },
    onSettingsUpdated: () => {},
    getServerPort: () => parseInt(window.location.port) || 8000,
    onServerPortUpdated: () => {},
    getServerAPIKey: async () => {
        const settings = await window.api.getSettings();
        return settings.apiKey?.value || '';
    },
    fetchWithApiKey: async (url) => {
        try {
            let apiKey = await window.api.getServerAPIKey();
            const options = {};
            if(apiKey != null && apiKey != '') {
                options.headers = {
                Authorization: `Bearer ${apiKey}`,
                }
            }
            return await fetch(url, options);
        } catch (e) {
            console.error('fetchWithApiKey error:', e);
            throw e;
        }
    },
    getVersion: async () => {
        try {
            const response = await window.api.fetchWithApiKey('/api/v1/health');
            if (response.ok) {
                const data = await response.json();
                return data.version || 'Unknown';
            } else {
                console.error('Health response not OK:', response.status, response.statusText);
            }
        } catch (e) {
            console.error('Failed to fetch version:', e);
        }
        return 'Unknown';
    },
    restartApp: () => window.location.reload(),
    getSystemStats: async () => {
        try {
            const response = await window.api.fetchWithApiKey('/api/v1/system-stats');
            if (response.ok) {
                return await response.json();
            }
        } catch (e) {
            console.warn('Failed to fetch system stats:', e);
        }
        return { cpu_percent: null, memory_gb: 0, gpu_percent: null, vram_gb: null };
    },
    getSystemInfo: async () => {
        try {
            const response = await window.api.fetchWithApiKey('/api/v1/system-info');
            if (response.ok) {
                const data = await response.json();
                let maxGttGb = 0;
                let maxVramGb = 0;

                const considerAmdGpu = (gpu) => {
                    if (gpu && typeof gpu.virtual_mem_gb === 'number' && isFinite(gpu.virtual_mem_gb)) {
                        maxGttGb = Math.max(maxGttGb, gpu.virtual_mem_gb);
                    }
                    if (gpu && typeof gpu.vram_gb === 'number' && isFinite(gpu.vram_gb)) {
                        maxVramGb = Math.max(maxVramGb, gpu.vram_gb);
                    }
                };

                if (data.devices?.amd_igpu) {
                    considerAmdGpu(data.devices.amd_igpu);
                }
                if (Array.isArray(data.devices?.amd_dgpu)) {
                    data.devices.amd_dgpu.forEach(considerAmdGpu);
                }

                // Transform server response to match the About window format
                const systemInfo = {
                    system: 'Unknown',
                    os: data['OS Version'] || 'Unknown',
                    cpu: data['Processor'] || 'Unknown',
                    gpus: [],
                    gtt_gb: maxGttGb > 0 ? `${maxGttGb} GB` : undefined,
                    vram_gb: maxVramGb > 0 ? `${maxVramGb} GB` : undefined,
                };

                // Extract GPU information from devices
                if (data.devices) {
                    if (data.devices.amd_igpu?.name) {
                        systemInfo.gpus.push(data.devices.amd_igpu.name);
                    }
                    if (data.devices.nvidia_igpu?.name) {
                        systemInfo.gpus.push(data.devices.nvidia_igpu.name);
                    }
                    if (Array.isArray(data.devices.amd_dgpu)) {
                        data.devices.amd_dgpu.forEach(gpu => {
                            if (gpu.name) systemInfo.gpus.push(gpu.name);
                        });
                    }
                    if (Array.isArray(data.devices.nvidia_dgpu)) {
                        data.devices.nvidia_dgpu.forEach(gpu => {
                            if (gpu.name) systemInfo.gpus.push(gpu.name);
                        });
                    }
                }

                return systemInfo;
            }
        } catch (e) {
            console.warn('Failed to fetch system info:', e);
        }
        return { system: 'Unknown', os: 'Unknown', cpu: 'Unknown', gpus: [], gtt_gb: undefined, vram_gb: undefined };
    }
};
</script>
)";

            // Insert mock API before the closing </head> tag
            size_t head_end_pos = html.find("</head>");
            if (head_end_pos != std::string::npos) {
                html.insert(head_end_pos, mock_api);
            }

            // Set no-cache headers
            res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
            res.set_header("Pragma", "no-cache");
            res.set_header("Expires", "0");
            res.set_content(html, "text/html");
        };

        // Serve the web app's index.html at root and for SPA routes
        web_server.Get("/", serve_web_app_html);

        // Also serve at /web-app for backwards compatibility
        web_server.Get("/web-app/?", serve_web_app_html);

        // Serve all static assets from the web app directory (JS, CSS, fonts, assets, etc.)
        // Handle both root-level assets and /web-app/ prefixed paths for backwards compatibility
        auto serve_web_app_asset = [web_app_dir](const httplib::Request& req, httplib::Response& res, const std::string& file_path) {
            std::string full_path = web_app_dir + "/" + file_path;

            // Serve the file
            std::ifstream file(full_path, std::ios::binary);
            if (!file.is_open()) {
                res.status = 404;
                res.set_content("File not found", "text/plain");
                return;
            }

            // Read file content
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            file.close();

            // Determine content type based on extension
            std::string content_type = "application/octet-stream";
            size_t dot_pos = file_path.rfind('.');
            if (dot_pos != std::string::npos) {
                std::string ext = file_path.substr(dot_pos);
                if (ext == ".js") content_type = "text/javascript";
                else if (ext == ".css") content_type = "text/css";
                else if (ext == ".html") content_type = "text/html";
                else if (ext == ".woff") content_type = "font/woff";
                else if (ext == ".woff2") content_type = "font/woff2";
                else if (ext == ".ttf") content_type = "font/ttf";
                else if (ext == ".svg") content_type = "image/svg+xml";
                else if (ext == ".png") content_type = "image/png";
                else if (ext == ".jpg" || ext == ".jpeg") content_type = "image/jpeg";
                else if (ext == ".json") content_type = "application/json";
                else if (ext == ".ico") content_type = "image/x-icon";
            }

            res.set_content(content, content_type);
        };

        // Serve favicon from web-app directory at root
        web_server.Get("/favicon.ico", [serve_web_app_asset](const httplib::Request& req, httplib::Response& res) {
            serve_web_app_asset(req, res, "favicon.ico");
        });

        // Serve web app assets from root (for files like renderer.bundle.js, fonts, etc.)
        web_server.Get(R"(/([^/]+\.(js|css|woff|woff2|ttf|svg|png|jpg|jpeg|json|ico)))",
                      [serve_web_app_asset](const httplib::Request& req, httplib::Response& res) {
            std::string file_path = req.matches[1].str();
            serve_web_app_asset(req, res, file_path);
        });

        // Keep /web-app/ prefix routes for backwards compatibility
        web_server.Get(R"(/web-app/(.+))", [serve_web_app_asset](const httplib::Request& req, httplib::Response& res) {
            std::string file_path = req.matches[1].str();
            serve_web_app_asset(req, res, file_path);
        });

        std::cout << "[Server] Web app UI available at root (/) from: " << web_app_dir << std::endl;

        // SPA fallback: serve index.html for any unmatched GET routes that don't start with /api, /v0, /v1, /static, or /live
        // This enables client-side routing
        web_server.Get(R"(^(?!/api|/v0|/v1|/static|/live|/status|/internal).*)",
                      [serve_web_app_html](const httplib::Request& req, httplib::Response& res) {
            // Only serve index.html if the path doesn't look like a file with extension
            std::string path = req.path;
            size_t last_slash = path.rfind('/');
            std::string last_segment = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;

            // If the last segment has an extension and it's not .html, let it 404
            // (This helps catch missing assets more clearly)
            size_t dot_pos = last_segment.rfind('.');
            if (dot_pos != std::string::npos) {
                std::string ext = last_segment.substr(dot_pos);
                if (ext != ".html" && ext != ".htm") {
                    // File with extension not found, return 404
                    res.status = 404;
                    return;
                }
            }

            // Otherwise, serve the SPA index.html for client-side routing
            serve_web_app_html(req, res);
        });
    } else {
        // Fallback to static page when web-app is not compiled
        std::cout << "[Server] Web app directory not found at: " << web_app_dir << std::endl;
        std::cout << "[Server] Falling back to static status page at root" << std::endl;

        // Serve the static status page at root instead
        web_server.Get("/", serve_index_html);

        // Serve favicon from static directory
        web_server.Get("/favicon.ico", [static_dir](const httplib::Request& req, httplib::Response& res) {
            std::ifstream ifs(static_dir + "/favicon.ico", std::ios::binary);
            if (ifs) {
                std::string content((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
                res.set_content(content, "image/x-icon");
                res.status = 200;
            } else {
                res.set_content("Favicon not found.", "text/plain");
                res.status = 404;
            }
        });
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
            // Only set generic "endpoint not found" if no content was already set
            // This preserves specific error messages (e.g., "model not found")
            if (res.body.empty()) {
                nlohmann::json error = {
                    {"error", {
                        {"message", "The requested endpoint does not exist"},
                        {"type", "not_found"},
                        {"path", req.path}
                    }}
                };
                res.set_content(error.dump(), "application/json");
            }
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
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) {
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
    // Add request logging for ALL requests (except health checks and stats endpoints)
    web_server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        // Skip logging health checks and stats endpoints to reduce log noise
        if (req.path != "/api/v0/health" && req.path != "/api/v1/health" &&
            req.path != "/v0/health" && req.path != "/v1/health" && req.path != "/live" &&
            req.path != "/api/v0/system-stats" && req.path != "/api/v1/system-stats" &&
            req.path != "/v0/system-stats" && req.path != "/v1/system-stats" &&
            req.path != "/api/v0/stats" && req.path != "/api/v1/stats" &&
            req.path != "/v0/stats" && req.path != "/v1/stats") {
            std::cout << "[Server] " << req.method << " " << req.path << " - " << res.status << std::endl;
        }
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

    //For now we will use getLocalHostname to get the machines hostname.
    //This allows external devices to not have to do a rDNS lookup.
    bool RFC1918_IP = udp_beacon_.isRFC1918(ipv4);
    if(RFC1918_IP && !no_broadcast_) {
        udp_beacon_.startBroadcasting(
            8000, //Broadcast port best to not make it adjustable, so clients dont have to scan.
            udp_beacon_.buildStandardPayloadPattern
            (
                udp_beacon_.getLocalHostname(),
                "http://" + ipv4 + ":" + std::to_string(port_) + "/api/v1/"
            ),
            2
        );
    }
    else if (RFC1918_IP && no_broadcast_) {
        std::cout << "[Server] [Net Broadcast] Broadcasting disabled by --no-broadcast option" << std::endl;
    }
    else {
        std::cout << "[Server] [Net Broadcast] Unable to broadcast my existance please use a RFC1918 IPv4," << std::endl
                    << "[Server] [Net Broadcast] or hostname that resolves to RFC1918 IPv4." << std::endl;
    }

    if(http_v4_thread_.joinable())
        http_v4_thread_.join();
    if(http_v6_thread_.joinable())
        http_v6_thread_.join();
}

void Server::stop() {
    if (running_) {
        std::cout << "[Server] Stopping HTTP server..." << std::endl;
        udp_beacon_.stopBroadcasting();
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

// Helper function to generate detailed model-not-found error responses
// ====================================================================
// Generates an actionable error message for model loading failures.
// Handles three cases:
//   1. Model exists but was filtered out (e.g., NPU model on non-NPU system)
//   2. Model doesn't exist in the registry at all
//   3. Model exists but failed to load (engine error)
nlohmann::json Server::create_model_error(const std::string& requested_model, const std::string& exception_msg) {
    nlohmann::json error_response;

    // Case 1: Check if this model exists but was filtered out due to system requirements
    std::string filter_reason = model_manager_->get_model_filter_reason(requested_model);

    if (!filter_reason.empty()) {
        // Model exists but is not available on this system
        std::string message = "Model '" + requested_model + "' is not available on this system. " + filter_reason;

        error_response["error"] = {
            {"message", message},
            {"type", "model_not_supported"},
            {"param", "model"},
            {"code", "model_not_supported"},
            {"requested_model", requested_model}
        };

        return error_response;
    }

    // Case 2: Check if model doesn't exist in the registry at all
    if (!model_manager_->model_exists(requested_model)) {
        std::string message = "Model '" + requested_model + "' was not found. ";

        // Get available models and suggest some
        auto available_models = model_manager_->get_supported_models();

        if (!available_models.empty()) {
            // Collect model names
            std::vector<std::string> model_names;
            model_names.reserve(available_models.size());
            for (const auto& [name, info] : available_models) {
                model_names.push_back(name);
            }

            // Sort alphabetically for consistent output
            std::sort(model_names.begin(), model_names.end());

            // Show up to 3 available models
            const size_t max_suggestions = 3;
            size_t count = std::min(model_names.size(), max_suggestions);

            message += "Available models include: ";
            for (size_t i = 0; i < count; ++i) {
                if (i > 0) message += ", ";
                message += "'" + model_names[i] + "'";
            }

            if (model_names.size() > max_suggestions) {
                message += ", and " + std::to_string(model_names.size() - max_suggestions) + " more";
            }
            message += ". ";
        }

        message += "Use 'lemonade-server list' or GET /api/v1/models?show_all=true to see all available models.";

        error_response["error"] = {
            {"message", message},
            {"type", "model_not_found"},
            {"param", "model"},
            {"code", "model_not_found"},
            {"requested_model", requested_model}
        };

        return error_response;
    }

    // Case 3: Model was invalidated by a backend upgrade (e.g., FLM version change)
    // This happens when FLM is upgraded and old model files are no longer compatible
    if (exception_msg.find("was invalidated") != std::string::npos) {
        std::string message = "Model '" + requested_model + "' needs to be re-downloaded. " +
            "The FLM backend was upgraded and the previously downloaded model files are no longer compatible. " +
            "Please use 'lemonade-server pull " + requested_model + "' or click Download in the UI to re-download this model.";

        error_response["error"] = {
            {"message", message},
            {"type", "model_invalidated"},
            {"param", "model"},
            {"code", "model_invalidated"},
            {"requested_model", requested_model}
        };

        return error_response;
    }

    // Case 4: Model exists and is available, but failed to load (engine error)
    // Return the actual exception message so the user knows what went wrong
    std::string message = "Failed to load model '" + requested_model + "': " + exception_msg;

    error_response["error"] = {
        {"message", message},
        {"type", "model_load_error"},
        {"param", "model"},
        {"code", "model_load_error"},
        {"requested_model", requested_model}
    };

    return error_response;
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
    router_->load_model(requested_model, info, RecipeOptions(info.recipe, json::object()), true);
    std::cout << "[Server] Model loaded successfully: " << requested_model << std::endl;
}

void Server::handle_health(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    nlohmann::json response = {{"status", "ok"}};

    // Add version information
    response["version"] = LEMON_VERSION_STRING;

    // Add model loaded information like Python implementation
    std::string loaded_model = router_->get_loaded_model();

    response["model_loaded"] = loaded_model.empty() ? nlohmann::json(nullptr) : nlohmann::json(loaded_model);

    // Multi-model support: Add all loaded models
    response["all_models_loaded"] = router_->get_all_loaded_models();

    // Add max model limits
    response["max_models"] = router_->get_max_model_limits();

    // Add log streaming support information
    response["log_streaming"] = {
        {"sse", true},
        {"websocket", false}  // WebSocket support not yet implemented
    };

    res.set_content(response.dump(), "application/json");
}

void Server::handle_live(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    // liveness response
    static const char* kLiveResponse = R"({"status":"ok"})";

    res.set_content(kLiveResponse, "application/json");
    res.status = 200;
}

void Server::handle_models(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

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
        response["data"].push_back(model_info_to_json(model_id, model_info));
    }

    res.set_content(response.dump(), "application/json");
}

nlohmann::json Server::model_info_to_json(const std::string& model_id, const ModelInfo& info) {
    nlohmann::json model_json = {
        {"id", model_id},
        {"object", "model"},
        {"created", 1234567890},
        {"owned_by", "lemonade"},
        {"checkpoint", info.checkpoint},
        {"recipe", info.recipe},
        {"downloaded", info.downloaded},
        {"suggested", info.suggested},
        {"labels", info.labels},
        {"recipe_options", info.recipe_options.to_json()},
    };

    // Add size if available
    if (info.size > 0.0) {
        model_json["size"] = info.size;
    }

    // Add image_defaults if present (for sd-cpp models)
    if (info.image_defaults.has_defaults) {
        model_json["image_defaults"] = {
            {"steps", info.image_defaults.steps},
            {"cfg_scale", info.image_defaults.cfg_scale},
            {"width", info.image_defaults.width},
            {"height", info.image_defaults.height}
        };
    }

    return model_json;
}

void Server::handle_model_by_id(const httplib::Request& req, httplib::Response& res) {
    std::string model_id = req.matches[1];

    if (model_manager_->model_exists(model_id)) {
        auto info = model_manager_->get_model_info(model_id);
        res.set_content(model_info_to_json(model_id, info).dump(), "application/json");
    } else {
        res.status = 404;
        auto error_response = create_model_error(model_id, "Model not found");
        res.set_content(error_response.dump(), "application/json");
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
                auto error_response = create_model_error(requested_model, e.what());
                // Set appropriate status code based on error type
                std::string error_code = error_response["error"]["code"].get<std::string>();
                if (error_code == "model_load_error" || error_code == "model_invalidated") {
                    res.status = 500;  // Internal server error - model exists but failed to load
                } else {
                    res.status = 404;  // Not found - model doesn't exist or is filtered out
                }
                res.set_content(error_response.dump(), "application/json");
                return;
            }
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }

        // Check if the loaded model supports chat completion (only LLM models do)
        std::string model_to_check = request_json.contains("model") ? request_json["model"].get<std::string>() : "";
        if (router_->get_model_type(model_to_check) != ModelType::LLM) {
            std::cerr << "[Server ERROR] Model does not support chat completion" << std::endl;
            res.status = 400;
            res.set_content(R"({"error": {"message": "This model does not support chat completion. Only LLM models support this endpoint.", "type": "invalid_request_error"}})", "application/json");
            return;
        }

        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();

        // Use original request body - each backend (FLM, llamacpp, etc.) handles
        // model name transformation internally via their forward methods
        std::string request_body = req.body;
        bool request_modified = false;

        // Handle enable_thinking=false by prepending /no_think to last user message
        if (request_json.contains("enable_thinking") &&
            request_json["enable_thinking"].is_boolean() &&
            request_json["enable_thinking"].get<bool>() == false) {

            if (request_json.contains("messages") && request_json["messages"].is_array()) {
                auto& messages = request_json["messages"];

                // Find the last user message (iterate backwards)
                for (int i = messages.size() - 1; i >= 0; i--) {
                    if (messages[i].is_object() &&
                        messages[i].contains("role") &&
                        messages[i]["role"].is_string() &&
                        messages[i]["role"].get<std::string>() == "user") {

                        // Prepend /no_think to the content
                        if (messages[i].contains("content") && messages[i]["content"].is_string()) {
                            std::string original_content = messages[i]["content"].get<std::string>();
                            messages[i]["content"] = "/no_think\n" + original_content;
                            request_modified = true;
                            break;
                        }
                    }
                }
            }
        }

        // If we modified the request, serialize it back to string
        if (request_modified) {
            request_body = request_json.dump();
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
                auto error_response = create_model_error(requested_model, e.what());
                // Set appropriate status code based on error type
                std::string error_code = error_response["error"]["code"].get<std::string>();
                if (error_code == "model_load_error" || error_code == "model_invalidated") {
                    res.status = 500;  // Internal server error - model exists but failed to load
                } else {
                    res.status = 404;  // Not found - model doesn't exist or is filtered out
                }
                res.set_content(error_response.dump(), "application/json");
                return;
            }
        } else if (!router_->is_model_loaded()) {
            std::cerr << "[Server ERROR] No model loaded and no model specified in request" << std::endl;
            res.status = 400;
            res.set_content("{\"error\": \"No model loaded and no model specified in request\"}", "application/json");
            return;
        }

        // Check if the loaded model supports completion (only LLM models do)
        std::string model_to_check = request_json.contains("model") ? request_json["model"].get<std::string>() : "";
        if (router_->get_model_type(model_to_check) != ModelType::LLM) {
            std::cerr << "[Server ERROR] Model does not support completion" << std::endl;
            res.status = 400;
            res.set_content(R"({"error": {"message": "This model does not support completion. Only LLM models support this endpoint.", "type": "invalid_request_error"}})", "application/json");
            return;
        }

        // Check if streaming is requested
        bool is_streaming = request_json.contains("stream") && request_json["stream"].get<bool>();

        // Use original request body - each backend handles model name transformation internally
        std::string request_body = req.body;

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

                        return false;
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
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = (error_code == "model_load_error" || error_code == "model_invalidated") ? 500 : 404;
                res.set_content(error_response.dump(), "application/json");
                return;
            }
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
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = (error_code == "model_load_error" || error_code == "model_invalidated") ? 500 : 404;
                res.set_content(error_response.dump(), "application/json");
                return;
            }
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

void Server::handle_audio_transcriptions(const httplib::Request& req, httplib::Response& res) {
    try {
        std::cout << "[Server] POST /api/v1/audio/transcriptions" << std::endl;

        // OpenAI audio API uses multipart form data
        if (!req.is_multipart_form_data()) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Request must be multipart/form-data"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Build request JSON for router
        nlohmann::json request_json;

        // Extract form fields
        if (req.form.has_field("model")) {
            request_json["model"] = req.form.get_field("model");
        }
        if (req.form.has_field("language")) {
            request_json["language"] = req.form.get_field("language");
        }
        if (req.form.has_field("prompt")) {
            request_json["prompt"] = req.form.get_field("prompt");
        }
        if (req.form.has_field("response_format")) {
            request_json["response_format"] = req.form.get_field("response_format");
        }
        if (req.form.has_field("temperature")) {
            request_json["temperature"] = std::stod(req.form.get_field("temperature"));
        }

        // Extract audio file
        const auto& files = req.form.files;
        bool found_audio = false;
        for (const auto& file_pair : files) {
            if (file_pair.first == "file") {
                const auto& file = file_pair.second;
                request_json["file_data"] = file.content;
                request_json["filename"] = file.filename;
                found_audio = true;
                std::cout << "[Server] Audio file: " << file.filename
                          << " (" << file.content.size() << " bytes)" << std::endl;
                break;
            }
        }

        if (!found_audio) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'file' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Handle model loading
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load audio model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = (error_code == "model_load_error" || error_code == "model_invalidated") ? 500 : 404;
                res.set_content(error_response.dump(), "application/json");
                return;
            }
        } else {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'model' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Forward to router
        auto response = router_->audio_transcriptions(request_json);

        // Check for error in response
        if (response.contains("error")) {
            res.status = 500;
        }

        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_audio_transcriptions: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "internal_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_audio_speech(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        // Handle model loading
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load text-to-speech model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = (error_code == "model_load_error" || error_code == "model_invalidated") ? 500 : 404;
                res.set_content(error_response.dump(), "application/json");
                return;
            }
        } else {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'model' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        if (!request_json.contains("input")) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'input' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        bool is_streaming = (request_json.contains("stream") && request_json["stream"].get<bool>());

        if (request_json.contains("stream_format")) {
            is_streaming = true;
            if (request_json["stream_format"] != "audio") {
                res.status = 400;
                nlohmann::json error = {{"error", {
                    {"message", "Only pcm audio streaming format is supported"},
                    {"type", "invalid_request_error"}
                }}};
                res.set_content(error.dump(), "application/json");
                return;
            }
        }

        std::string mime_type;
        if (is_streaming) {
            mime_type = MIME_TYPES["pcm"];
        } else if (request_json.contains("response_format")) {
            if (MIME_TYPES.contains(request_json["response_format"])) {
                mime_type = MIME_TYPES[request_json["response_format"]];
            } else {
                nlohmann::json error = {{"error", {
                    {"message", "Unsupported audio format requested"},
                    {"type", "invalid_request_error"}
                }}};
                res.set_content(error.dump(), "application/json");
                return;
            }
        } else {
            mime_type = MIME_TYPES["mp3"];
        }

        // Log the HTTP request
        std::cout << "[Server] POST /api/v1/audio/speech" << std::endl;

        res.set_header("Content-Type", mime_type);

        auto audio_source = [this, request_json](size_t offset, httplib::DataSink& sink) {
            // For chunked responses, offset tracks bytes sent so far
            // We only want to stream once when offset is 0
            if (offset > 0) {
                return false; // We're done after the first call
            }

            // Use unified Router path for streaming
            router_->audio_speech(request_json, sink);

            return false;
        };

        if (is_streaming) {
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no"); // Disable nginx buffering

            // Use cpp-httplib's chunked content provider for streaming
            res.set_chunked_content_provider(mime_type, audio_source);
        } else {
            res.set_content_provider(mime_type, audio_source);
        }

        return;
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_audio_speech: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "internal_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_image_generations(const httplib::Request& req, httplib::Response& res) {
    try {
        std::cout << "[Server] POST /api/v1/images/generations" << std::endl;

        auto request_json = nlohmann::json::parse(req.body);

        // Validate required fields
        if (!request_json.contains("prompt")) {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'prompt' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Handle model loading
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load image model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = (error_code == "model_load_error" || error_code == "model_invalidated") ? 500 : 404;
                res.set_content(error_response.dump(), "application/json");
                return;
            }
        } else {
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", "Missing 'model' field in request"},
                {"type", "invalid_request_error"}
            }}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Forward to router
        auto response = router_->image_generations(request_json);

        // Check for error in response
        if (response.contains("error")) {
            res.status = 500;
        }

        res.set_content(response.dump(), "application/json");

    } catch (const nlohmann::json::exception& e) {
        std::cerr << "[Server] JSON parse error in handle_image_generations: " << e.what() << std::endl;
        res.status = 400;
        nlohmann::json error = {{"error", {
            {"message", "Invalid JSON: " + std::string(e.what())},
            {"type", "invalid_request_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_image_generations: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", {
            {"message", e.what()},
            {"type", "internal_error"}
        }}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_responses(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);

        // Handle model loading/switching using helper function
        if (request_json.contains("model")) {
            std::string requested_model = request_json["model"];
            try {
                auto_load_model_if_needed(requested_model);
            } catch (const std::exception& e) {
                std::cerr << "[Server ERROR] Failed to load model: " << e.what() << std::endl;
                auto error_response = create_model_error(requested_model, e.what());
                std::string error_code = error_response["error"]["code"].get<std::string>();
                res.status = (error_code == "model_load_error" || error_code == "model_invalidated") ? 500 : 404;
                res.set_content(error_response.dump(), "application/json");
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
        bool embedding = request_json.value("embedding", false);
        bool reranking = request_json.value("reranking", false);
        bool image = request_json.value("image", false);
        std::string mmproj = request_json.value("mmproj", "");
        bool do_not_upgrade = request_json.value("do_not_upgrade", false);
        bool stream = request_json.value("stream", false);

        std::cout << "[Server] Pulling model: " << model_name << std::endl;
        if (!checkpoint.empty()) {
            std::cout << "[Server]   checkpoint: " << checkpoint << std::endl;
        }
        if (!recipe.empty()) {
            std::cout << "[Server]   recipe: " << recipe << std::endl;
        }

        // Validate: if checkpoint or recipe are provided, model name must have "user." prefix
        if (!checkpoint.empty() || !recipe.empty()) {
            if (model_name.substr(0, 5) != "user.") {
                res.status = 400;
                nlohmann::json error = {{"error",
                    "When providing 'checkpoint' or 'recipe', the model name must include the "
                    "`user.` prefix, for example `user.Phi-4-Mini-GGUF`. Received: " + model_name}};
                res.set_content(error.dump(), "application/json");
                return;
            }
        }

        // Local import mode: CLI has already copied files to HF cache, just resolve and register
        bool local_import = request_json.value("local_import", false);
        if (local_import) {
            std::string hf_cache = model_manager_->get_hf_cache_dir();
            std::string model_name_clean = model_name.substr(5); // Remove "user." prefix
            std::replace(model_name_clean.begin(), model_name_clean.end(), '/', '-');
            std::string dest_path = hf_cache + "/models--" + model_name_clean;

            std::cout << "[Server] Local import mode - resolving files in: " << dest_path << std::endl;

            resolve_and_register_local_model(
                dest_path, model_name, recipe, "", mmproj,
                reasoning, vision, embedding, reranking, image, hf_cache
            );

            nlohmann::json response = {
                {"status", "success"},
                {"model_name", model_name},
                {"message", "Model imported and registered successfully"}
            };
            res.set_content(response.dump(), "application/json");
            return;
        }

        if (stream) {
            // SSE streaming mode - send progress events
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");

            res.set_chunked_content_provider(
                "text/event-stream",
                [this, model_name, checkpoint, recipe, reasoning, vision,
                 embedding, reranking, image, mmproj, do_not_upgrade](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) {
                        return false; // Already sent everything
                    }

                    try {
                        // Create progress callback that emits SSE events
                        // Returns false if client disconnects to cancel download
                        DownloadProgressCallback progress_cb = [&sink](const DownloadProgress& p) -> bool {
                            nlohmann::json event_data;
                            event_data["file"] = p.file;
                            event_data["file_index"] = p.file_index;
                            event_data["total_files"] = p.total_files;
                            // Explicitly cast to uint64_t for proper JSON serialization
                            event_data["bytes_downloaded"] = static_cast<uint64_t>(p.bytes_downloaded);
                            event_data["bytes_total"] = static_cast<uint64_t>(p.bytes_total);
                            event_data["percent"] = p.percent;

                            std::string event;
                            if (p.complete) {
                                event = "event: complete\ndata: " + event_data.dump() + "\n\n";
                            } else {
                                event = "event: progress\ndata: " + event_data.dump() + "\n\n";
                            }

                            // Check if client is still connected
                            // sink.write() returns false when client disconnects
                            if (!sink.write(event.c_str(), event.size())) {
                                std::cout << "[Server] Client disconnected, cancelling download" << std::endl;
                                return false;  // Cancel download
                            }
                            return true;  // Continue download
                        };

                        model_manager_->download_model(model_name, checkpoint, recipe,
                                                      reasoning, vision, embedding, reranking, image,
                                                      mmproj, do_not_upgrade, progress_cb);

                    } catch (const std::exception& e) {
                        // Send error event (only if it's not a cancellation)
                        std::string error_msg = e.what();
                        if (error_msg != "Download cancelled") {
                            nlohmann::json error_data = {{"error", error_msg}};
                            std::string event = "event: error\ndata: " + error_data.dump() + "\n\n";
                            sink.write(event.c_str(), event.size());
                        }
                    }

                    // Explicitly signal we're done - this ensures proper chunked encoding termination
                    sink.done();
                    return false; // Signal completion
                });
        } else {
            // Legacy synchronous mode - blocks until complete
            model_manager_->download_model(model_name, checkpoint, recipe,
                                          reasoning, vision, embedding, reranking, image, mmproj, do_not_upgrade);

            nlohmann::json response = {{"status", "success"}, {"model_name", model_name}};
            res.set_content(response.dump(), "application/json");
        }

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

    // Declare model_name outside try block so it's available in catch block
    std::string model_name;

    try {
        auto request_json = nlohmann::json::parse(req.body);
        model_name = request_json["model_name"];

        // Get model info
        if (!model_manager_->model_exists(model_name)) {
            std::cerr << "[Server ERROR] Model not found: " << model_name << std::endl;
            res.status = 404;
            auto error_response = create_model_error(model_name, "Model not found");
            res.set_content(error_response.dump(), "application/json");
            return;
        }

        auto info = model_manager_->get_model_info(model_name);

        // Extract optional per-model settings (defaults to -1 / empty = use Router defaults)
        RecipeOptions options = RecipeOptions(info.recipe, request_json);
        bool save_options = request_json.value("save_options", false);

        if (router_->is_model_loaded(model_name)) {
            router_->unload_model(model_name);
            std::cout << "[Server] Reloading model: " << model_name;
        } else {
            std::cout << "[Server] Loading model: " << model_name;
        }
        std::cout << " " << options.to_log_string(false);
        std::cout << std::endl;

        // Persist request options to model info if requested
        if (save_options) {
            info.recipe_options = options;
            model_manager_->save_model_options(info);
        }

        // Download model if needed (first-time use)
        if (!info.downloaded) {
            std::cout << "[Server] Model not downloaded, downloading..." << std::endl;
            model_manager_->download_model(model_name);
            info = model_manager_->get_model_info(model_name);
        }

        // Load model with optional per-model settings
        router_->load_model(model_name, info, options, true);

        // Return success response
        nlohmann::json response = {
            {"status", "success"},
            {"model_name", model_name},
            {"checkpoint", info.checkpoint},
            {"recipe", info.recipe}
        };
        res.set_content(response.dump(), "application/json");

    } catch (const std::exception& e) {
        std::cerr << "[Server ERROR] Failed to load model: " << e.what() << std::endl;

        // Use consistent error format
        if (!model_name.empty()) {
            auto error_response = create_model_error(model_name, e.what());
            std::string error_code = error_response["error"]["code"].get<std::string>();
            res.status = (error_code == "model_load_error" || error_code == "model_invalidated") ? 500 : 404;
            res.set_content(error_response.dump(), "application/json");
        } else {
            // JSON parsing failed before we got model_name - return generic error
            res.status = 400;
            nlohmann::json error = {{"error", {
                {"message", std::string("Invalid request: ") + e.what()},
                {"type", "invalid_request_error"},
                {"code", "invalid_request"}
            }}};
            res.set_content(error.dump(), "application/json");
        }
    }
}

void Server::handle_unload(const httplib::Request& req, httplib::Response& res) {
    try {
        std::cout << "[Server] Unload request received" << std::endl;
        std::cout << "[Server] Request method: " << req.method << ", body length: " << req.body.length() << std::endl;
        std::cout << "[Server] Content-Type: " << req.get_header_value("Content-Type") << std::endl;

        // Multi-model support: Optional model_name parameter
        std::string model_name;
        if (!req.body.empty()) {
            try {
                auto request_json = nlohmann::json::parse(req.body);
                if (request_json.contains("model_name") && request_json["model_name"].is_string()) {
                    model_name = request_json["model_name"].get<std::string>();
                } else if (request_json.contains("model") && request_json["model"].is_string()) {
                    model_name = request_json["model"].get<std::string>();
                }
            } catch (...) {
                // Ignore parse errors, just unload all
            }
        }

        router_->unload_model(model_name);  // Empty string = unload all

        if (model_name.empty()) {
            std::cout << "[Server] All models unloaded successfully" << std::endl;
            nlohmann::json response = {
                {"status", "success"},
                {"message", "All models unloaded successfully"}
            };
            res.status = 200;
            res.set_content(response.dump(), "application/json");
        } else {
            std::cout << "[Server] Model '" << model_name << "' unloaded successfully" << std::endl;
            nlohmann::json response = {
                {"status", "success"},
                {"message", "Model unloaded successfully"},
                {"model_name", model_name}
            };
            res.status = 200;
            res.set_content(response.dump(), "application/json");
        }
    } catch (const std::exception& e) {
        std::cerr << "[Server ERROR] Unload failed: " << e.what() << std::endl;

        // Check if error is "Model not loaded" for 404
        std::string error_msg = e.what();
        if (error_msg.find("not loaded") != std::string::npos) {
            res.status = 404;
        } else {
            res.status = 500;
        }

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

        // If the model is currently loaded, unload it first to release file locks
        if (router_->is_model_loaded(model_name)) {
            std::cout << "[Server] Model is loaded, unloading before delete: " << model_name << std::endl;
            router_->unload_model(model_name);
        }

        // Retry delete with delays to handle in-progress downloads releasing file handles
        // This handles the race condition where a cancelled download hasn't yet released
        // its file handles when the delete request arrives
        const int max_retries = 3;
        const int retry_delay_seconds = 5;
        std::string last_error;

        for (int attempt = 0; attempt <= max_retries; ++attempt) {
            try {
                model_manager_->delete_model(model_name);

                // Success - send response and return
                nlohmann::json response = {
                    {"status", "success"},
                    {"message", "Deleted model: " + model_name}
                };
                res.set_content(response.dump(), "application/json");
                return;

            } catch (const std::exception& e) {
                last_error = e.what();

                // Only retry on "file in use" type errors (Windows and POSIX patterns)
                bool is_file_locked =
                    last_error.find("being used by another process") != std::string::npos ||
                    last_error.find("Permission denied") != std::string::npos ||
                    last_error.find("resource busy") != std::string::npos;

                if (is_file_locked && attempt < max_retries) {
                    std::cout << "[Server] Delete failed (file in use), retry "
                              << (attempt + 1) << "/" << max_retries
                              << " in " << retry_delay_seconds << "s..." << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(retry_delay_seconds));
                    continue;
                }

                // Non-retryable error or max retries exceeded - rethrow
                throw;
            }
        }

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

// Helper function to resolve model files and register a local model
// Called by handle_pull when local_import=true
// Parameters:
//   - dest_path: Directory where model files are located (already copied/uploaded)
//   - model_name: Model name with "user." prefix
//   - recipe: Inference recipe (llamacpp, ryzenai-llm, whispercpp)
//   - variant: Optional variant hint for GGUF file selection
//   - mmproj: Optional mmproj filename hint
//   - reasoning, vision, embedding, reranking, image: Model labels
//   - hf_cache: HuggingFace cache directory for computing relative paths
void Server::resolve_and_register_local_model(
    const std::string& dest_path,
    const std::string& model_name,
    const std::string& recipe,
    const std::string& variant,
    const std::string& mmproj,
    bool reasoning,
    bool& vision,  // May be modified if mmproj found
    bool embedding,
    bool reranking,
    bool image,
    const std::string& hf_cache) {

    std::string resolved_checkpoint;
    std::string resolved_mmproj;

    // For RyzenAI LLM models, find genai_config.json
    if (recipe == "ryzenai-llm") {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dest_path)) {
            if (entry.is_regular_file() && entry.path().filename() == "genai_config.json") {
                resolved_checkpoint = entry.path().parent_path().string();
                break;
            }
        }
        if (resolved_checkpoint.empty()) {
            resolved_checkpoint = dest_path;
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

            for (const auto& entry : std::filesystem::recursive_directory_iterator(dest_path)) {
                if (entry.is_regular_file() && entry.path().filename() == search_term) {
                    gguf_file_found = entry.path().string();
                    break;
                }
            }
        }

        // If no variant or variant not found, search for any .gguf file (excluding mmproj)
        if (gguf_file_found.empty()) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dest_path)) {
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

        resolved_checkpoint = gguf_file_found.empty() ? dest_path : gguf_file_found;
    }
    // For whispercpp, find .bin file
    else if (recipe == "whispercpp") {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dest_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.find(".bin") != std::string::npos) {
                    resolved_checkpoint = entry.path().string();
                    break;
                }
            }
        }
        if (resolved_checkpoint.empty()) {
            resolved_checkpoint = dest_path;
        }
    }

    // Search for mmproj file if vision is enabled or mmproj hint provided
    if (vision || !mmproj.empty()) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dest_path)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                std::string filename_lower = filename;
                std::transform(filename_lower.begin(), filename_lower.end(), filename_lower.begin(), ::tolower);

                // Match either the provided mmproj name or any mmproj file
                if (!mmproj.empty() && filename == mmproj) {
                    resolved_mmproj = filename;
                    vision = true;  // Ensure vision is set
                    break;
                } else if (filename_lower.find("mmproj") != std::string::npos) {
                    resolved_mmproj = filename;
                    vision = true;  // Ensure vision is set
                    break;
                }
            }
        }
    }

    // Build checkpoint for registration - store as relative path from HF cache
    std::string checkpoint_to_register;
    if (!resolved_checkpoint.empty()) {
        std::filesystem::path rel = std::filesystem::relative(resolved_checkpoint, hf_cache);
        checkpoint_to_register = rel.string();
    } else {
        // Fallback - use dest_path relative to hf_cache
        std::filesystem::path rel = std::filesystem::relative(dest_path, hf_cache);
        checkpoint_to_register = rel.string();
    }

    std::cout << "[Server] Registering model with checkpoint: " << checkpoint_to_register << std::endl;

    // Register the model with source to mark how it was added
    model_manager_->register_user_model(
        model_name,
        checkpoint_to_register,
        recipe,
        reasoning,
        vision,
        embedding,
        reranking,
        image,
        resolved_mmproj.empty() ? mmproj : resolved_mmproj,
        "local_upload"
    );

    std::cout << "[Server] Model registered successfully" << std::endl;
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

    // Get system info - this function handles all errors internally and never throws
    nlohmann::json system_info = SystemInfoCache::get_system_info_with_cache();
    res.set_content(system_info.dump(), "application/json");
}

// Helper: Get CPU usage percentage
double Server::get_cpu_usage() {
#ifdef __linux__
    // Linux: Parse /proc/stat for system-wide CPU usage
    std::lock_guard<std::mutex> lock(cpu_stats_mutex_);

    std::ifstream stat_file("/proc/stat");
    if (!stat_file.is_open()) {
        return -1.0;
    }

    std::string line;
    std::getline(stat_file, line);
    stat_file.close();

    // Parse: "cpu  user nice system idle iowait irq softirq steal"
    std::istringstream iss(line);
    std::string cpu_label;
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;

    iss >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

    uint64_t total_idle = idle + iowait;
    uint64_t total_active = user + nice + system + irq + softirq + steal;
    uint64_t total = total_idle + total_active;

    if (last_cpu_stats_.total > 0) {
        uint64_t idle_diff = total_idle - last_cpu_stats_.total_idle;
        uint64_t total_diff = total - last_cpu_stats_.total;

        last_cpu_stats_.total_idle = total_idle;
        last_cpu_stats_.total = total;

        if (total_diff > 0) {
            return ((total_diff - idle_diff) * 100.0) / total_diff;
        }
    }

    last_cpu_stats_.total_idle = total_idle;
    last_cpu_stats_.total = total;
    return 0.0; // First call, no delta yet

#elif defined(_WIN32)
    // Windows: Use GetSystemTimes for system-wide CPU usage
    std::lock_guard<std::mutex> lock(cpu_stats_mutex_);

    FILETIME idle_time, kernel_time, user_time;
    if (!GetSystemTimes(&idle_time, &kernel_time, &user_time)) {
        return -1.0;
    }

    // Convert FILETIME to uint64_t (100-nanosecond intervals)
    auto filetime_to_uint64 = [](const FILETIME& ft) -> uint64_t {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };

    uint64_t idle = filetime_to_uint64(idle_time);
    uint64_t kernel = filetime_to_uint64(kernel_time); // Includes idle time
    uint64_t user = filetime_to_uint64(user_time);

    // Kernel time includes idle time, so subtract it to get actual kernel time
    uint64_t total = kernel + user;
    uint64_t total_idle = idle;

    if (last_cpu_stats_.total > 0) {
        uint64_t idle_diff = total_idle - last_cpu_stats_.total_idle;
        uint64_t total_diff = total - last_cpu_stats_.total;

        last_cpu_stats_.total_idle = total_idle;
        last_cpu_stats_.total = total;

        if (total_diff > 0) {
            return ((total_diff - idle_diff) * 100.0) / total_diff;
        }
    }

    last_cpu_stats_.total_idle = total_idle;
    last_cpu_stats_.total = total;
    return 0.0; // First call, no delta yet

#elif defined(__APPLE__)
    // macOS: Could use host_processor_info or top command
    return -1.0; // Not implemented yet

#else
    return -1.0;
#endif
}

// Helper: Get GPU usage percentage (AMD GPUs on Linux)
double Server::get_gpu_usage() {
#ifdef __linux__
    // Linux: Read from AMD sysfs (gpu_busy_percent)
    // Check all GPUs and return the highest utilization
    try {
        std::string drm_path = "/sys/class/drm";

        if (!fs::exists(drm_path)) {
            return -1.0;
        }

        double highest_usage = -1.0;

        for (const auto& entry : fs::directory_iterator(drm_path)) {
            std::string card_name = entry.path().filename().string();
            if (card_name.find("card") != 0 || card_name.find("-") != std::string::npos) {
                continue;
            }

            std::string busy_path = entry.path().string() + "/device/gpu_busy_percent";
            std::ifstream busy_file(busy_path);
            if (busy_file.is_open()) {
                double usage;
                busy_file >> usage;
                busy_file.close();
                if (usage > highest_usage) {
                    highest_usage = usage;
                }
            }
        }

        return highest_usage;
    } catch (...) {
        return -1.0;
    }

#else
    // GPU usage monitoring not implemented for Windows/macOS
    return -1.0;
#endif
}

// Helper: Get VRAM/GTT usage in GB (AMD GPUs on Linux)
double Server::get_vram_usage() {
#ifdef __linux__
    // Linux: Read from AMD sysfs
    // For dGPU: return VRAM used
    // For APU: return VRAM + GTT used
    // On multi-GPU systems, return memory from GPU with highest utilization
    try {
        std::string drm_path = "/sys/class/drm";

        if (!fs::exists(drm_path)) {
            return -1.0;
        }

        double highest_usage = -1.0;
        std::string highest_card;
        double highest_card_memory = 0.0;

        for (const auto& entry : fs::directory_iterator(drm_path)) {
            std::string card_name = entry.path().filename().string();
            if (card_name.find("card") != 0 || card_name.find("-") != std::string::npos) {
                continue;
            }

            std::string device_path = entry.path().string() + "/device";

            // Read GPU utilization to find the most active GPU
            double gpu_usage = 0.0;
            std::ifstream busy_file(device_path + "/gpu_busy_percent");
            if (busy_file.is_open()) {
                busy_file >> gpu_usage;
                busy_file.close();
            }

            // Check if this is a dGPU (has board_info) or APU (no board_info)
            bool is_dgpu = fs::exists(device_path + "/board_info");

            // Read VRAM used
            uint64_t vram_used = 0;
            std::ifstream vram_file(device_path + "/mem_info_vram_used");
            if (vram_file.is_open()) {
                vram_file >> vram_used;
                vram_file.close();
            }

            // Read GTT used
            uint64_t gtt_used = 0;
            std::ifstream gtt_file(device_path + "/mem_info_gtt_used");
            if (gtt_file.is_open()) {
                gtt_file >> gtt_used;
                gtt_file.close();
            }

            // Skip if no memory info found
            if (vram_used == 0 && gtt_used == 0) {
                continue;
            }

            // Calculate memory for this card
            uint64_t card_memory = is_dgpu ? vram_used : (vram_used + gtt_used);

            // Track the GPU with highest utilization
            if (gpu_usage > highest_usage || highest_usage < 0) {
                highest_usage = gpu_usage;
                highest_card = card_name;
                highest_card_memory = card_memory / (1024.0 * 1024.0 * 1024.0); // Convert to GB
            }
        }

        return highest_card_memory > 0 ? highest_card_memory : -1.0;
    } catch (...) {
        return -1.0;
    }

#else
    // VRAM monitoring not implemented for Windows/macOS
    return -1.0;
#endif
}

void Server::handle_system_stats(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }

    nlohmann::json stats;

    // CPU usage
    double cpu_percent = get_cpu_usage();
    stats["cpu_percent"] = (cpu_percent >= 0) ? nlohmann::json(cpu_percent) : nlohmann::json();

    // Get memory info
#ifdef _WIN32
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        double used_gb = (memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
        stats["memory_gb"] = std::round(used_gb * 10.0) / 10.0;
    } else {
        stats["memory_gb"] = 0;
    }
#elif defined(__linux__)
    // Linux: Read /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        long long total_kb = 0, available_kb = 0;
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) {
                sscanf(line.c_str(), "MemTotal: %lld kB", &total_kb);
            } else if (line.find("MemAvailable:") == 0) {
                sscanf(line.c_str(), "MemAvailable: %lld kB", &available_kb);
                break;
            }
        }
        meminfo.close();
        double used_gb = (total_kb - available_kb) / (1024.0 * 1024.0);
        stats["memory_gb"] = std::round(used_gb * 10.0) / 10.0;
    } else {
        stats["memory_gb"] = 0;
    }
#elif defined(__APPLE__)
    // macOS: Get memory info
    int64_t physical_memory = 0;
    size_t length = sizeof(physical_memory);
    if (sysctlbyname("hw.memsize", &physical_memory, &length, nullptr, 0) == 0) {
        // For now, just report total memory since getting free memory is complex on macOS
        double total_gb = physical_memory / (1024.0 * 1024.0 * 1024.0);
        stats["memory_gb"] = std::round(total_gb * 10.0) / 10.0;
    } else {
        stats["memory_gb"] = 0;
    }
#else
    stats["memory_gb"] = 0;
#endif

    // GPU usage
    double gpu_percent = get_gpu_usage();
    stats["gpu_percent"] = (gpu_percent >= 0) ? nlohmann::json(gpu_percent) : nlohmann::json();

    // VRAM usage
    double vram_gb = get_vram_usage();
    stats["vram_gb"] = (vram_gb >= 0) ? nlohmann::json(vram_gb) : nlohmann::json();

    res.set_content(stats.dump(), "application/json");
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
