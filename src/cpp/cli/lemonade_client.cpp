#include "lemon_cli/lemonade_client.h"
#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace lemonade {

using json = nlohmann::json;

LemonadeClient::LemonadeClient(const std::string& host, int port, const std::string& api_key)
    : host_(host), port_(port), api_key_(api_key) {}

LemonadeClient::~LemonadeClient() {}

std::string LemonadeClient::normalize_host(const std::string& host) const {
    if (host.empty() || host == "0.0.0.0" || host == "localhost") {
        return "127.0.0.1";
    }
    return host;
}

// Helper to create and configure httplib::Client (timeouts in milliseconds)
static httplib::Client make_client(const std::string& host, int port, const std::string& api_key,
                                    int connection_timeout_ms = 30000, int read_timeout_ms = 30000) {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(connection_timeout_ms / 1000, (connection_timeout_ms % 1000) * 1000);
    cli.set_read_timeout(read_timeout_ms / 1000, (read_timeout_ms % 1000) * 1000);

    if (api_key != "") {
        cli.set_bearer_token_auth(api_key);
    }
    return cli;
}

static void assert_http_ok(const httplib::Result& res) {
    if (!res) {
        throw std::runtime_error(
            "Could not connect to Lemonade server (" + httplib::to_string(res.error()) + ").\n"
            "Make sure the server is running and try again.");
    } else if (res->status == 401) {
        throw std::runtime_error("Forbidden by the server. Did you set the API key?");
    } else if (res->status != 200) {
        throw std::runtime_error("Request failed: " + std::to_string(res->status));
    }
}

// Overloaded make_request with configurable timeouts (in milliseconds)
std::string LemonadeClient::make_request(const std::string& path, const std::string& method,
                                          const std::string& body, const std::string& content_type,
                                          int connection_timeout_ms, int read_timeout_ms) const {
    std::string normalized_host = normalize_host(host_);
    httplib::Client cli = make_client(normalized_host, port_, api_key_, connection_timeout_ms, read_timeout_ms);

    httplib::Result res;

    if (method == "GET") {
        res = cli.Get(path);
    } else if (method == "POST") {
        res = cli.Post(path, body, content_type);
    } else {
        throw std::runtime_error("Unsupported HTTP method: " + method);
    }

    assert_http_ok(res);
    return res->body;

}

// Helper function to handle SSE streaming response
static httplib::Result handle_sse_stream(httplib::Client& cli, const std::string& path, const std::string& body, const std::string& content_type,
                              std::function<void(const std::string& event_type, const std::string& event_data)> callback) {
    std::string buffer;

    auto res = cli.Post(path, httplib::Headers(), body, content_type,
        [&](const char* data, size_t len) {
            buffer.append(data, len);

            size_t pos;
            while ((pos = buffer.find("\n\n")) != std::string::npos) {
                std::string message = buffer.substr(0, pos);
                buffer.erase(0, pos + 2);

                std::string event_type;
                std::string event_data;

                std::istringstream stream(message);
                std::string line;
                while (std::getline(stream, line)) {
                    if (line.substr(0, 6) == "event:") {
                        event_type = line.substr(7);
                        while (!event_type.empty() && event_type[0] == ' ') {
                            event_type.erase(0, 1);
                        }
                    } else if (line.substr(0, 5) == "data:") {
                        event_data = line.substr(6);
                        while (!event_data.empty() && event_data[0] == ' ') {
                            event_data.erase(0, 1);
                        }
                    }
                }

                if (!event_data.empty()) {
                    callback(event_type, event_data);
                }
            }

            return true;
        });

    return res;
}

// Overloaded make_request for streaming SSE responses (timeouts in milliseconds)
bool LemonadeClient::make_request(const std::string& path, const std::string& method,
                                   const std::string& body, const std::string& content_type,
                                   std::function<void(const std::string& event_type, const std::string& event_data)> callback,
                                   int connection_timeout_ms, int read_timeout_ms) const {
    std::string normalized_host = normalize_host(host_);
    httplib::Client cli = make_client(normalized_host, port_, api_key_, connection_timeout_ms, read_timeout_ms);

    if (method == "POST") {
        auto res = handle_sse_stream(cli, path, body, content_type, callback);
        assert_http_ok(res);

        return true;
    }

    throw std::runtime_error("Streaming only supports POST method");
}

int LemonadeClient::status(int display_port) const {
    try {
        std::string response = make_request("/api/v1/health", "GET", "", "", 500, 500);
        auto json_response = json::parse(response);

        int port = display_port > 0 ? display_port : port_;
        std::cout << "Server is running on port " << port << std::endl;
        std::cout << std::endl;

        // Server info table
        std::cout << std::left << std::setw(20) << "Property" << "Value" << std::endl;
        std::cout << std::string(50, '-') << std::endl;

        if (json_response.contains("version")) {
            std::cout << std::left << std::setw(20) << "Version"
                      << json_response["version"].get<std::string>() << std::endl;
        }
        if (json_response.contains("websocket_port")) {
            std::cout << std::left << std::setw(20) << "WebSocket Port"
                      << json_response["websocket_port"].get<int>() << std::endl;
        }
        if (json_response.contains("max_models") && json_response["max_models"].is_object()) {
            std::cout << std::left << std::setw(20) << "Max Models/Type"
                      << json_response["max_models"]["llm"].get<int>() << std::endl;
        }

        // Loaded models table
        if (json_response.contains("all_models_loaded") && json_response["all_models_loaded"].is_array() &&
            !json_response["all_models_loaded"].empty()) {
            std::cout << std::endl;
            std::cout << std::left
                      << std::setw(30) << "Model"
                      << std::setw(10) << "Type"
                      << std::setw(10) << "Device"
                      << std::setw(14) << "Recipe"
                      << "Checkpoint" << std::endl;
            std::cout << std::string(100, '-') << std::endl;

            for (const auto& model : json_response["all_models_loaded"]) {
                if (!model.is_object()) continue;

                std::cout << std::left
                          << std::setw(30) << model.value("model_name", "-")
                          << std::setw(10) << model.value("type", "-")
                          << std::setw(10) << model.value("device", "-")
                          << std::setw(14) << model.value("recipe", "-")
                          << model.value("checkpoint", "-") << std::endl;
            }
        } else {
            std::cout << std::endl;
            std::cout << "No models loaded." << std::endl;
        }

        return 0;

    } catch (const json::exception& e) {
        std::cerr << "Error parsing health response JSON: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        const std::string error = e.what();
        if (error.find("Connection failed:") == 0) {
            std::cerr << "Server is not running" << std::endl;
        } else {
            std::cerr << "Error fetching health status: " << error << std::endl;
        }
        return 1;
    }
}

std::vector<ModelInfo> LemonadeClient::get_models(bool show_all) const {
    std::vector<ModelInfo> models;

    try {
        std::string response = make_request("/api/v1/models?show_all=" + std::string(show_all ? "true" : "false"));
        auto json_response = json::parse(response);

        if (!json_response.contains("data") || !json_response["data"].is_array()) {
            return models;
        }

        for (const auto& model_item : json_response["data"]) {
            ModelInfo info;

            if (model_item.contains("id") && model_item["id"].is_string()) {
                info.id = model_item["id"].get<std::string>();
            }

            if (model_item.contains("checkpoint") && model_item["checkpoint"].is_string()) {
                info.checkpoint = model_item["checkpoint"].get<std::string>();
            }

            if (model_item.contains("recipe") && model_item["recipe"].is_string()) {
                info.recipe = model_item["recipe"].get<std::string>();
            }

            if (model_item.contains("downloaded") && model_item["downloaded"].is_boolean()) {
                info.downloaded = model_item["downloaded"].get<bool>();
            }

            if (!info.id.empty()) {
                models.push_back(info);
            }
        }

    } catch (const json::exception& e) {
        std::cerr << "Error parsing models JSON: " << e.what() << std::endl;
    }

    return models;
}

int LemonadeClient::list_models(bool show_all) const {
    try {
        std::vector<ModelInfo> models = get_models(show_all);

        if (models.empty()) {
            std::cout << "No models available" << std::endl;
            return 0;
        }

        std::cout << std::left << std::setw(40) << "Model Name"
                  << std::setw(12) << "Downloaded"
                  << "Details" << std::endl;
        std::cout << std::string(100, '-') << std::endl;

        for (const auto& model : models) {
            std::string downloaded = model.downloaded ? "Yes" : "No";
            std::string details = model.recipe.empty() ? "-" : model.recipe;

            std::cout << std::left << std::setw(40) << model.id
                      << std::setw(12) << downloaded
                      << details << std::endl;
        }

        std::cout << std::string(100, '-') << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error listing models: " << e.what() << std::endl;
        return 1;
    }
}

// Helper function to parse SSE progress events
static std::string format_speed(double bytes_per_sec) {
    if (bytes_per_sec < 1024.0) {
        return std::to_string(static_cast<int>(bytes_per_sec)) + " B/s";
    } else if (bytes_per_sec < 1024.0 * 1024.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (bytes_per_sec / 1024.0) << " KB/s";
        return oss.str();
    } else if (bytes_per_sec < 1024.0 * 1024.0 * 1024.0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (bytes_per_sec / (1024.0 * 1024.0)) << " MB/s";
        return oss.str();
    } else {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << (bytes_per_sec / (1024.0 * 1024.0 * 1024.0)) << " GB/s";
        return oss.str();
    }
}

static bool parse_sse_progress(const std::string& event_data, StreamingRequestState& state) {
    std::string& last_file = state.last_file;
    int& last_percent = state.last_percent;
    std::string& error_message = state.error_message;
    bool& total_size_printed = state.total_size_printed;
    uint64_t& last_file_size = state.last_file_size;
    try {
        auto json_data = json::parse(event_data);

        if (json_data.contains("file") && json_data["file"].is_string()) {
            std::string file = json_data["file"].get<std::string>();
            int file_index = json_data.value("file_index", 0);
            int total_files = json_data.value("total_files", 0);
            // Use uint64_t explicitly to avoid JSON type inference issues with large numbers
            uint64_t bytes_downloaded = json_data.value("bytes_downloaded", (uint64_t)0);
            uint64_t bytes_total = json_data.value("bytes_total", (uint64_t)0);
            uint64_t bytes_prev = json_data.value("bytes_previously_downloaded", (uint64_t)0);
            uint64_t total_download_size = json_data.value("total_download_size", (uint64_t)0);

            // Print total download size once on first event
            if (!total_size_printed && total_download_size > 0 && total_files > 0) {
                double size_gb = total_download_size / (1024.0 * 1024.0 * 1024.0);
                std::cout << "Total: " << std::fixed << std::setprecision(1)
                          << size_gb << " GB, " << total_files << " files" << std::endl;
                total_size_printed = true;
            }

            if (file != last_file) {
                // First event for this file — remember its size for progress display
                last_file_size = bytes_total;

                // Add newline after progress bar (carriage-return based), but not after "(already downloaded)"
                if (!last_file.empty() && last_percent != 100) {
                    std::cout << std::endl;
                }
                std::cout << "[" << file_index << "/" << total_files << "] " << file;
                if (bytes_total > 0) {
                    std::cout << " (" << std::fixed << std::setprecision(1)
                            << (bytes_total / (1024.0 * 1024.0)) << " MB)";
                }

                // Check if already downloaded: bytes_prev equals the KNOWN file size
                bool is_already_downloaded = bytes_prev > 0 && bytes_prev == bytes_total;
                if (is_already_downloaded) {
                    std::cout << " (already downloaded)";
                    last_percent = 100;
                } else {
                    last_percent = -1;
                    state.file_start_time = std::chrono::steady_clock::now();
                    state.file_bytes_resumed = bytes_prev;
                }
                std::cout << std::endl;
                last_file = file;
            } else if (last_percent != 100 && bytes_prev > 0 && bytes_prev == last_file_size) {
                // Completion event: bytes_prev matches known file size (not a redirect artifact)
                std::cout << "\r" << std::string(80, ' ') << "\r  (already downloaded)" << std::endl;
                last_percent = 100;
            }

            // Show progress bar using the known file size as denominator
            if (last_percent != 100 && last_file_size > 0) {
                int display_percent = static_cast<int>((bytes_downloaded * 100) / last_file_size);
                if (display_percent > 100) display_percent = 100;
                if (display_percent != last_percent) {
                    // Calculate speed from session bytes only (exclude resumed bytes)
                    std::string speed_str;
                    auto elapsed = std::chrono::steady_clock::now() - state.file_start_time;
                    double elapsed_sec = std::chrono::duration<double>(elapsed).count();
                    if (elapsed_sec > 0.5 && bytes_downloaded > state.file_bytes_resumed) {
                        double speed = (bytes_downloaded - state.file_bytes_resumed) / elapsed_sec;
                        speed_str = " " + format_speed(speed);
                    }

                    std::cout << "\r  Progress: " << display_percent << "% ("
                            << std::fixed << std::setprecision(1)
                            << (bytes_downloaded / (1024.0 * 1024.0)) << "/"
                            << (last_file_size / (1024.0 * 1024.0)) << " MB)"
                            << speed_str << "    " << std::flush;
                    last_percent = display_percent;
                }
            }
        }

        if (json_data.contains("error") && json_data["error"].is_string()) {
            error_message = json_data["error"].get<std::string>();
        }

        return json_data.contains("complete");
    } catch (const json::exception&) {
        return false;
    }
}

int LemonadeClient::pull_model(const json& model_data) {
    try {
        // Validate that model field exists in model_data
        if (!model_data.contains("model_name") || !model_data["model_name"].is_string()) {
            std::cerr << "Error: 'model_name' field is required in model_data" << std::endl;
            return 1;
        }

        std::string model_name = model_data["model_name"].get<std::string>();
        std::cout << "Pulling model: " << model_name << std::endl;

        json request_body = model_data;
        request_body["stream"] = true;

        std::string body = request_body.dump();

        StreamingRequestState state;

        make_request("/api/v1/pull", "POST", body, "application/json",
        [&](const std::string& event_type, const std::string& event_data) {
            if (event_type == "complete") {
                std::cout << std::endl;
                state.success = true;
            } else if (event_type == "error") {
                try {
                    auto error_json = json::parse(event_data);
                    if (error_json.contains("error")) {
                        state.error_message = error_json["error"].get<std::string>();
                    }
                } catch (...) {
                    state.error_message = event_data;
                }
            } else {
                parse_sse_progress(event_data, state);
            }
        }, 86400000, 30000);

        if (!state.success) {
            if (!state.error_message.empty()) {
                throw std::runtime_error(state.error_message);
            }

            throw std::runtime_error("Model pull failed");
        }

        std::cout << "Model pulled successfully: " << model_name << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error pulling model: " << e.what() << std::endl;
        return 1;
    }
}

int LemonadeClient::delete_model(const std::string& model_name) const {
    std::cout << "Deleting model: " << model_name << std::endl;

    try {
        json request_body = {{"model_name", model_name}};
        std::string response = make_request("/api/v1/delete", "POST", request_body.dump(), "application/json");

        auto response_json = json::parse(response);
        if (response_json.contains("status") && response_json["status"] == "success") {
            std::cout << "Model deleted successfully: " << model_name << std::endl;
            return 0;
        } else {
            std::cerr << "Failed to delete model" << std::endl;
            return 1;
        }

    } catch (const json::exception& e) {
        std::cerr << "Error parsing delete response JSON: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error deleting model: " << e.what() << std::endl;
        return 1;
    }
}

int LemonadeClient::load_model(const std::string& model_name, const nlohmann::json& recipe_options, bool save_options) const {
    std::cout << "Loading model: " << model_name << std::endl;

    try {
        json request_body = recipe_options;
        request_body["model_name"] = model_name;
        request_body["save_options"] = save_options;

        make_request("/api/v1/load", "POST", request_body.dump(), "application/json");

        std::cout << "Model loaded successfully!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error loading model: " << e.what() << std::endl;
        return 1;
    }
}

int LemonadeClient::unload_model(const std::string& model_name) const {
    try {
        json request_body = {};

        if (model_name.empty()) {
            std::cout << "Unloading all models" << std::endl;
        } else {
            std::cout << "Unloading model: " << model_name << std::endl;
            request_body["model_name"] = model_name;
        }

        make_request("/api/v1/unload", "POST", request_body.dump(), "application/json");

        std::cout << "Model unloaded successfully!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error unloading model: " << e.what() << std::endl;
        return 1;
    }
}

nlohmann::json LemonadeClient::get_model_info(const std::string& model_name) const {
    try {
        std::string response = make_request("/api/v1/models/" + model_name);
        return json::parse(response);
    } catch (const json::exception& e) {
        std::cerr << "Error parsing model info JSON: " << e.what() << std::endl;
        return json{};
    } catch (const std::exception& e) {
        std::cerr << "Error fetching model info: " << e.what() << std::endl;
        return json{};
    }
}

int LemonadeClient::list_recipes() const {
    try {
        std::string response = make_request("/api/v1/system-info");
        auto json_response = json::parse(response);

        std::vector<RecipeStatus> recipes;

        if (!json_response.contains("recipes") || !json_response["recipes"].is_object()) {
            std::cout << "No recipes available" << std::endl;
            return 0;
        }

        for (const auto& [recipe_name, recipe_data] : json_response["recipes"].items()) {
            RecipeStatus status;
            status.name = recipe_name;

            if (recipe_data.contains("backends") && recipe_data["backends"].is_object()) {
                for (const auto& [backend_name, backend_data] : recipe_data["backends"].items()) {
                    BackendStatus backend;
                    backend.name = backend_name;

                    if (backend_data.contains("state") && backend_data["state"].is_string()) {
                        backend.state = backend_data["state"].get<std::string>();
                    }

                    if (backend_data.contains("version") && backend_data["version"].is_string()) {
                        backend.version = backend_data["version"].get<std::string>();
                    }

                    if (backend_data.contains("message") && backend_data["message"].is_string()) {
                        backend.message = backend_data["message"].get<std::string>();
                    }

                    if (backend_data.contains("action") && backend_data["action"].is_string()) {
                        backend.action = backend_data["action"].get<std::string>();
                    }

                    status.backends.push_back(backend);
                }
            }

            recipes.push_back(status);
        }

        std::cout << std::left << std::setw(20) << "Recipe"
                  << std::setw(12) << "Backend"
                  << std::setw(16) << "Status"
                  << std::setw(46) << "Message/Version"
                  << "Action" << std::endl;
        std::cout << std::string(148, '-') << std::endl;

        for (const auto& recipe : recipes) {
            bool first_backend = true;

            if (recipe.backends.empty()) {
                std::cout << std::left << std::setw(20) << recipe.name
                          << std::setw(12) << "-"
                          << std::setw(16) << "unsupported"
                          << std::setw(46) << "No backend definitions"
                          << "-" << std::endl;
            } else {
                for (const auto& backend : recipe.backends) {
                    std::string recipe_col = first_backend ? recipe.name : "";
                    std::string status_str = backend.state.empty() ? "unsupported" : backend.state;

                    std::string info_col;
                    if (status_str == "installed" && !backend.version.empty() && backend.version != "unknown") {
                        info_col = backend.version;
                    } else if (!backend.message.empty()) {
                        info_col = backend.message;
                    } else {
                        info_col = "-";
                    }
                    std::string action_col = backend.action.empty() ? "-" : backend.action;

                    std::cout << std::left << std::setw(20) << recipe_col
                              << std::setw(12) << backend.name
                              << std::setw(16) << status_str
                              << std::setw(46) << info_col
                              << " " << action_col << std::endl;

                    first_backend = false;
                }
            }
        }

        std::cout << std::string(148, '-') << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error listing recipes: " << e.what() << std::endl;
        return 1;
    }
}

int LemonadeClient::install_backend(const std::string& recipe, const std::string& backend) {
    std::cout << "Installing backend: " << recipe << ":" << backend << std::endl;

    try {
        json request_body = {{"recipe", recipe}, {"backend", backend}};
        request_body["stream"] = true;
        std::string body = request_body.dump();

        StreamingRequestState state;

        make_request("/api/v1/install", "POST", body, "application/json",
        [&](const std::string& event_type, const std::string& event_data) {
            if (event_type == "complete") {
                std::cout << std::endl;
                state.success = true;
            } else if (event_type == "error") {
                // Server sent an explicit error event
                try {
                    auto error_json = json::parse(event_data);
                    if (error_json.contains("error")) {
                        state.error_message = error_json["error"].get<std::string>();
                    }
                } catch (...) {
                    state.error_message = event_data;
                }
            } else {
                parse_sse_progress(event_data, state);
            }
        }, 86400000, 30000);
        if (!state.success) {
            if (!state.error_message.empty()) {
                throw std::runtime_error(state.error_message);
            }
            throw std::runtime_error("Backend installation failed (no details from server)");
        }

        std::cout << "Backend installed successfully: " << recipe << ":" << backend << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

int LemonadeClient::uninstall_backend(const std::string& recipe, const std::string& backend) {
    std::cout << "Uninstalling backend: " << recipe << ":" << backend << std::endl;

    try {
        json request_body = {{"recipe", recipe}, {"backend", backend}};
        std::string response = make_request("/api/v1/uninstall", "POST", request_body.dump(), "application/json");

        auto response_json = json::parse(response);
        if (response_json.contains("status") && response_json["status"] == "success") {
            std::cout << "Backend uninstalled successfully: " << recipe << ":" << backend << std::endl;
            return 0;
        } else {
            std::cerr << "Uninstall failed: " << response << std::endl;
            return 1;
        }

    } catch (const json::exception& e) {
        std::cerr << "Error parsing uninstall response JSON: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error uninstalling backend: " << e.what() << std::endl;
        return 1;
    }
}

} // namespace lemonade
