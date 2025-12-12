// Model endpoint handlers for lemon::Server
// These are Server methods extracted to a separate file for organization.
// Handlers: health, models, model_by_id, pull, load, unload, delete, params, add_local_model

#include "lemon/server.h"
#include "lemon/version.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace lemon {

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
    std::string loaded_checkpoint = router_->get_loaded_checkpoint();
    std::string loaded_model = router_->get_loaded_model();
    
    response["checkpoint_loaded"] = loaded_checkpoint.empty() ? nlohmann::json(nullptr) : nlohmann::json(loaded_checkpoint);
    response["model_loaded"] = loaded_model.empty() ? nlohmann::json(nullptr) : nlohmann::json(loaded_model);
    
    // Multi-model support: Add all loaded models
    response["all_models_loaded"] = router_->get_all_loaded_models();
    
    // Add max model limits
    response["max_models"] = router_->get_max_model_limits();
    
    // Add context size
    response["context_size"] = router_->get_ctx_size();
    
    // Add log streaming support information
    response["log_streaming"] = {
        {"sse", true},
        {"websocket", false}  // WebSocket support not yet implemented
    };
    
    res.set_content(response.dump(), "application/json");
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
        {"labels", info.labels}
    };
    
    // Add size if available
    if (info.size > 0.0) {
        model_json["size"] = info.size;
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
        res.set_content("{\"error\": \"Model not found\"}", "application/json");
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
        
        if (stream) {
            // SSE streaming mode - send progress events
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");
            
            res.set_chunked_content_provider(
                "text/event-stream",
                [this, model_name, checkpoint, recipe, reasoning, vision, 
                 embedding, reranking, mmproj, do_not_upgrade](size_t offset, httplib::DataSink& sink) {
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
                                                      reasoning, vision, embedding, reranking, 
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
                    
                    return false; // Signal completion
                });
        } else {
            // Legacy synchronous mode - blocks until complete
            model_manager_->download_model(model_name, checkpoint, recipe, 
                                          reasoning, vision, embedding, reranking, mmproj, do_not_upgrade);
            
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
    try {
        auto request_json = nlohmann::json::parse(req.body);
        std::string model_name = request_json["model_name"];
        
        // Extract optional per-model settings (defaults to -1 / empty = use Router defaults)
        int ctx_size = request_json.value("ctx_size", -1);
        std::string llamacpp_backend = request_json.value("llamacpp_backend", "");
        std::string llamacpp_args = request_json.value("llamacpp_args", "");
        
        std::cout << "[Server] Loading model: " << model_name;
        if (ctx_size > 0) std::cout << " (ctx_size=" << ctx_size << ")";
        if (!llamacpp_backend.empty()) std::cout << " (backend=" << llamacpp_backend << ")";
        if (!llamacpp_args.empty()) std::cout << " (args=" << llamacpp_args << ")";
        std::cout << std::endl;
        
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
        
        // Get model info
        if (!model_manager_->model_exists(model_name)) {
            throw std::runtime_error("Model not found: " + model_name);
        }
        
        auto info = model_manager_->get_model_info(model_name);
        
        // Download model if needed (first-time use)
        if (!info.downloaded) {
            std::cout << "[Server] Model not downloaded, downloading..." << std::endl;
            model_manager_->download_model(model_name);
            info = model_manager_->get_model_info(model_name);
        }
        
        // Load model with optional per-model settings
        router_->load_model(model_name, info, true, ctx_size, llamacpp_backend, llamacpp_args);
        
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
        bool embedding = false;
        bool reranking = false;
        
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
        if (req.form.has_field("embedding")) {
            std::string embedding_str = req.form.get_field("embedding");
            embedding = (embedding_str == "true" || embedding_str == "True" || embedding_str == "1");
        }
        if (req.form.has_field("reranking")) {
            std::string reranking_str = req.form.get_field("reranking");
            reranking = (reranking_str == "true" || reranking_str == "True" || reranking_str == "1");
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
        std::vector<std::string> valid_recipes = {"llamacpp", "oga-npu", "oga-hybrid", "oga-cpu", "whispercpp"};
        if (std::find(valid_recipes.begin(), valid_recipes.end(), recipe) == valid_recipes.end()) {
            res.status = 400;
            nlohmann::json error = {{"error", "Invalid recipe. Must be one of: llamacpp, oga-npu, oga-hybrid, oga-cpu, whispercpp"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // Check if model files are provided (or checkpoint path for whisper)
        const auto& files = req.form.files;
        bool is_whisper = (recipe == "whispercpp");
        if (files.empty() && !is_whisper) {
            res.status = 400;
            nlohmann::json error = {{"error", "No model files provided for upload"}};
            res.set_content(error.dump(), "application/json");
            return;
        }

        // For whisper models, checkpoint can be a local path
        if (is_whisper && !checkpoint.empty() && files.empty()) {
            // Use checkpoint as local path - validate it exists
            if (!std::filesystem::exists(checkpoint)) {
                res.status = 400;
                nlohmann::json error = {{"error", "Checkpoint file does not exist: " + checkpoint}};
                res.set_content(error.dump(), "application/json");
                return;
            }
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
        std::string hf_cache = model_manager_->get_hf_cache_dir();
        
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
        std::string source_type = "local_upload";

        // For whisper models with local checkpoint path (no files uploaded), use the path directly
        if (is_whisper && files.empty() && !checkpoint.empty()) {
            // Store absolute path for whisper local models
            checkpoint_to_register = checkpoint;
            source_type = "local_path";  // Use special source so it's resolved as-is
            std::cout << "[Server] Using local whisper model path: " << checkpoint_to_register << std::endl;
        } else if (!resolved_checkpoint.empty()) {
            std::filesystem::path rel = std::filesystem::relative(resolved_checkpoint, hf_cache);
            checkpoint_to_register = rel.string();
        } else {
            // Fallback if no files found - use directory path
            checkpoint_to_register = "models--" + repo_cache_name;
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
            resolved_mmproj.empty() ? mmproj : resolved_mmproj,
            source_type
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

} // namespace lemon

