#include "lemon/backends/external/external_backend_server.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/shell_utils.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <algorithm>

namespace lemon {

namespace fs = std::filesystem;

static std::string to_posix_path(const std::string& path_str) {
    std::string res = path_str;
    std::replace(res.begin(), res.end(), '\\', '/');
    return res;
}

ExternalBackendServer::ExternalBackendServer(const std::string& server_name)
    : WrappedServer(server_name, "info") {
}

ExternalBackendServer::~ExternalBackendServer() {
    unload();
}

bool ExternalBackendServer::has_capability(const std::string& cap_name) const {
    if (descriptor_) {
        return descriptor_->has_capability(cap_name);
    }
    return true;
}

bool ExternalBackendServer::supports_downsize() const {
    return descriptor_ && !descriptor_->downsize_endpoint.empty();
}

std::vector<std::string> ExternalBackendServer::resolve_command_args(
    const std::vector<std::string>& template_args,
    const std::unordered_map<std::string, std::string>& token_map,
    const RecipeOptions& recipe_options) {

    std::vector<std::string> resolved;

    auto replace_in_string = [&](const std::string& input, bool& has_unpopulated) -> std::string {
        std::string result;
        result.reserve(static_cast<size_t>(input.size() * 1.2));
        size_t pos = 0;
        has_unpopulated = false;
        while (pos < input.length()) {
            size_t open_brace = input.find('{', pos);
            if (open_brace == std::string::npos) {
                result.append(input.substr(pos));
                break;
            }
            result.append(input.substr(pos, open_brace - pos));
            size_t close_brace = input.find('}', open_brace);
            if (close_brace == std::string::npos) {
                result.append(input.substr(open_brace));
                break;
            }

            std::string key = input.substr(open_brace + 1, close_brace - open_brace - 1);
            bool is_valid_token_syntax = !key.empty();
            for (char c : key) {
                unsigned char uc = static_cast<unsigned char>(c);
                if (!isalnum(uc) && uc != '_' && uc != '-' && uc != ':' && uc != '.' && uc != '/' && uc != '=' && uc != ',' && uc != '"' && uc != '\'' && uc != ' ') {
                    is_valid_token_syntax = false;
                    break;
                }
            }

            if (!is_valid_token_syntax) {
                result.push_back('{');
                pos = open_brace + 1;
                continue;
            }

            auto it = token_map.find(key);
            if (it != token_map.end()) {
                result.append(it->second);
            } else if (key.rfind("env:", 0) == 0) {
                std::string var_spec = key.substr(4);
                std::string var_name = var_spec;
                std::string default_val = "";
                size_t colon_pos = var_spec.find(":-");
                if (colon_pos != std::string::npos) {
                    var_name = var_spec.substr(0, colon_pos);
                    default_val = var_spec.substr(colon_pos + 2);
                }
                const char* env_val = std::getenv(var_name.c_str());
                std::string val = (env_val && env_val[0] != '\0') ? env_val : default_val;
                result.append(val);
            } else if (key.rfind("custom:", 0) == 0) {
                std::string opt_spec = key.substr(7);
                std::string opt_name = opt_spec;
                std::string default_val = "";
                size_t colon_pos = opt_spec.find(":-");
                if (colon_pos != std::string::npos) {
                    opt_name = opt_spec.substr(0, colon_pos);
                    default_val = opt_spec.substr(colon_pos + 2);
                }
                json opt_val = recipe_options.get_option(opt_name);
                if (!opt_val.is_null()) {
                    result.append(opt_val.is_string() ? opt_val.get<std::string>() : opt_val.dump());
                } else if (!default_val.empty()) {
                    result.append(default_val);
                } else {
                    has_unpopulated = true;
                    result.append(input.substr(open_brace, close_brace - open_brace + 1));
                }
            } else {
                has_unpopulated = true;
                result.append(input.substr(open_brace, close_brace - open_brace + 1));
            }
            pos = close_brace + 1;
        }
        return result;
    };

    for (const auto& t_arg : template_args) {
        if (t_arg == "{custom_args}") {
            json args_val = recipe_options.get_option("args");
            if (!args_val.is_null()) {
                std::vector<std::string> custom_args_list;
                if (args_val.is_array()) {
                    for (const auto& el : args_val) {
                        if (el.is_string()) {
                            custom_args_list.push_back(el.get<std::string>());
                        } else {
                            custom_args_list.push_back(el.dump());
                        }
                    }
                } else {
                    std::string raw_args = args_val.is_string() ? args_val.get<std::string>() : args_val.dump();
                    custom_args_list = utils::parse_custom_args(raw_args);
                }

                if (descriptor_ && !descriptor_->protected_flags.empty()) {
                    for (const auto& carg : custom_args_list) {
                        for (const auto& pf : descriptor_->protected_flags) {
                            if (carg == pf || carg.rfind(pf + "=", 0) == 0) {
                                throw std::invalid_argument("Custom argument '" + carg + "' collides with protected flag '" + pf + "'");
                            }
                        }
                    }
                }
                resolved.insert(resolved.end(), custom_args_list.begin(), custom_args_list.end());
            }
            continue;
        }

        bool has_unpopulated = false;
        std::string res = replace_in_string(t_arg, has_unpopulated);
        if (has_unpopulated) {
            continue;
        }
        resolved.push_back(res);
    }

    return resolved;
}

void ExternalBackendServer::load(const std::string& model_name,
                                const ModelInfo& model_info,
                                const RecipeOptions& options,
                                bool do_not_upgrade) {
    (void)do_not_upgrade;
    auto start_time = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if ((state_ == ModelState::READY || state_ == ModelState::IN_USE) && is_backend_alive()) {
            return;
        }

        state_ = ModelState::LOADING;
        state_cv_.notify_all();
    }

    struct LoadStateGuard {
        ExternalBackendServer* server;
        bool success = false;
        ~LoadStateGuard() noexcept {
            if (!success) {
                try {
                    server->unload();
                } catch (const std::exception& e) {
                    LOG(ERROR, "LoadStateGuard") << "Unload failed during guard destruction: " << e.what();
                } catch (...) {
                    LOG(ERROR, "LoadStateGuard") << "Unknown error during guard destruction";
                }
            }
        }
    } state_guard{this, false};

    auto local_desc = lemon::backends::descriptor_shared_for(model_info.recipe);
    if (!local_desc) {
        throw std::runtime_error("No descriptor found for recipe: " + model_info.recipe);
    }
    set_descriptor(local_desc);

    std::string local_platform = "cpu";
    json dev_opt = options.get_option("device");
    std::string req_device = !dev_opt.is_null() ? (dev_opt.is_string() ? dev_opt.get<std::string>() : dev_opt.dump()) : device_type_to_string(model_info.device);
    std::transform(req_device.begin(), req_device.end(), req_device.begin(), [](unsigned char c){ return std::tolower(c); });

    if (!req_device.empty() && local_desc->platforms.count(req_device)) {
        local_platform = req_device;
    } else {
#ifdef __APPLE__
        if (local_desc->platforms.count("metal")) {
            local_platform = "metal";
        }
#else
        std::string cuda_arch = lemon::SystemInfo::get_cuda_arch();
        std::string rocm_arch = lemon::SystemInfo::get_rocm_arch();
        if (!cuda_arch.empty() && local_desc->platforms.count("cuda")) {
            local_platform = "cuda";
        } else if (!rocm_arch.empty() && local_desc->platforms.count("rocm")) {
            local_platform = "rocm";
        } else if (local_desc->platforms.count("vulkan")) {
            local_platform = "vulkan";
        } else if (local_desc->platforms.count("oneapi")) {
            local_platform = "oneapi";
        } else if (local_desc->platforms.count("tpu")) {
            local_platform = "tpu";
        }
#endif
    }
    if (!local_desc->platforms.count(local_platform)) {
        if (local_desc->platforms.count("cpu")) {
            local_platform = "cpu";
        } else if (!local_desc->platforms.empty()) {
            local_platform = local_desc->platforms.begin()->first;
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        selected_platform_ = local_platform;
        active_platform_config_ = descriptor_->platforms.at(selected_platform_);
        loaded_recipe_options_ = options;
    }

    port_ = choose_port();

    std::unordered_map<std::string, std::string> local_tokens;
    local_tokens["port"] = std::to_string(port_);
    local_tokens["host"] = "127.0.0.1";
    local_tokens["log_level"] = log_level_.empty() ? "info" : log_level_;
    local_tokens["model_name"] = model_name_;
    local_tokens["recipe"] = descriptor_->recipe;
    std::string main_cp_resolved = model_info.resolved_path("main");
    if (main_cp_resolved.empty() && model_manager_) {
        main_cp_resolved = model_manager_->resolve_model_path(model_info, "main", model_info.checkpoint("main"));
    }
    if (!main_cp_resolved.empty()) {
        checkpoint_ = main_cp_resolved;
    }

    local_tokens["resolved_path"] = checkpoint_;
    local_tokens["gguf_path"] = checkpoint_;

    std::string hf_cache = utils::get_hf_cache_dir();
    local_tokens["hf_cache"] = hf_cache;

    // lexically_relative (not fs::relative) avoids resolving symlinks - fs::relative
    // canonicalizes internally, which would rewrite HF-cache checkpoint symlinks down
    // to their extensionless blob path and break extension-sensitive loaders.
    auto is_safe_relative = [](const fs::path& rel_p) -> bool {
        if (rel_p.empty() || rel_p.is_absolute()) return false;
        if (rel_p.begin() != rel_p.end() && *rel_p.begin() == "..") return false;
        return true;
    };

    fs::path cp_path = utils::path_from_utf8(checkpoint_);
    fs::path hf_path = utils::path_from_utf8(hf_cache);
    fs::path rel_path = cp_path.lexically_relative(hf_path);
    if (is_safe_relative(rel_path)) {
        local_tokens["model_relative_path"] = to_posix_path(utils::path_to_utf8(rel_path));
    } else {
        local_tokens["model_relative_path"] = to_posix_path(utils::path_to_utf8(cp_path.filename()));
    }

    for (const auto& [cp_key, cp_val] : model_info.checkpoints) {
        std::string cp_resolved = model_info.resolved_path(cp_key);
        if (cp_resolved.empty() && model_manager_) {
            cp_resolved = model_manager_->resolve_model_path(model_info, cp_key, cp_val);
        }
        if (cp_resolved.empty()) {
            cp_resolved = cp_val;
        }
        local_tokens["checkpoint:" + cp_key] = cp_resolved;
        fs::path item_cp_path = utils::path_from_utf8(cp_resolved);
        fs::path item_rel_path = item_cp_path.lexically_relative(hf_path);
        std::string cp_rel_str = is_safe_relative(item_rel_path) ? utils::path_to_utf8(item_rel_path) : utils::path_to_utf8(item_cp_path.filename());
        local_tokens["checkpoint_relative:" + cp_key] = to_posix_path(cp_rel_str);
    }

    auto get_opt_str = [&](const std::string& key, const std::string& fallback = "") -> std::string {
        json val = options.get_option(key);
        if (val.is_null()) return fallback;
        if (val.is_string()) return val.get<std::string>();
        if (val.is_number_integer()) return std::to_string(val.get<int>());
        if (val.is_number_float()) return std::to_string(val.get<double>());
        if (val.is_boolean()) return val.get<bool>() ? "true" : "false";
        return val.dump();
    };

    local_tokens["ctx_size"] = get_opt_str("ctx_size", "2048");
    local_tokens["max_ctx"] = get_opt_str("ctx_size", "2048");
    local_tokens["batch_size"] = get_opt_str("batch_size", "512");
    local_tokens["ubatch_size"] = get_opt_str("ubatch_size", "512");
    local_tokens["threads"] = get_opt_str("threads", "4");
    local_tokens["cache_type_k"] = get_opt_str("cache_type_k", "f16");
    local_tokens["cache_type_v"] = get_opt_str("cache_type_v", "f16");
    local_tokens["target_device"] = get_opt_str("target_device", get_opt_str("gpu_id", "auto:0"));
    local_tokens["cuda_visible_devices"] = get_opt_str("cuda_visible_devices", get_opt_str("gpu_id", "0"));
    local_tokens["hip_visible_devices"] = get_opt_str("hip_visible_devices", get_opt_str("gpu_id", "0"));
    local_tokens["rocr_visible_devices"] = get_opt_str("rocr_visible_devices", get_opt_str("gpu_id", "0"));
    local_tokens["ggml_vk_visible_devices"] = get_opt_str("ggml_vk_visible_devices", get_opt_str("gpu_id", "0"));
    local_tokens["vk_visible_devices"] = get_opt_str("vk_visible_devices", get_opt_str("gpu_id", "0"));
    local_tokens["ze_affinity_mask"] = get_opt_str("ze_affinity_mask", get_opt_str("gpu_id", "0"));
    local_tokens["cache_dir"] = utils::get_cache_dir();

    std::string model_dir = checkpoint_.empty() ? "." : fs::path(checkpoint_).parent_path().string();
    std::string exe_dir = active_platform_config_.command.empty() ? "." : fs::path(active_platform_config_.command).parent_path().string();
    local_tokens["model_dir"] = to_posix_path(model_dir);
    local_tokens["exe_dir"] = to_posix_path(exe_dir);
    local_tokens["rocm_arch"] = lemon::SystemInfo::get_rocm_arch();
    local_tokens["cuda_arch"] = lemon::SystemInfo::get_cuda_arch();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        instance_token_map_ = local_tokens;
    }

    std::vector<std::string> final_args = resolve_command_args(active_platform_config_.args, local_tokens, options);

    std::unordered_map<std::string, std::string> env_vars;
    for (const auto& [k, v] : active_platform_config_.env) {
        std::vector<std::string> tmp{v};
        auto res = resolve_command_args(tmp, local_tokens, options);
        env_vars[k] = res.empty() ? "" : res[0];
    }

    std::vector<std::pair<std::string, std::string>> env_vec;
    for (const auto& [k, v] : env_vars) {
        env_vec.emplace_back(k, v);
    }

    std::string cmd_str = active_platform_config_.command;
    for (const auto& arg : final_args) {
        cmd_str += " " + arg;
    }
    LOG(INFO) << "ExternalBackendServer: Dispatched command: " << utils::sanitize_log_string(cmd_str);

    if ((!active_platform_config_.stop_command.empty() || !active_platform_config_.stop_command_args.empty()) && descriptor_) {
        try {
            std::vector<std::string> pre_stop_args = resolve_command_args(active_platform_config_.stop_command_args, local_tokens, options);
            std::vector<std::string> stop_cmd_template{active_platform_config_.stop_command};
            auto resolved_cmd = resolve_command_args(stop_cmd_template, local_tokens, options);
            std::string pre_stop_cmd = resolved_cmd.empty() ? "" : utils::escape_shell_arg(resolved_cmd[0]);

            for (const auto& arg : pre_stop_args) {
                if (!pre_stop_cmd.empty()) pre_stop_cmd += " ";
                pre_stop_cmd += utils::escape_shell_arg(arg);
            }
            if (!pre_stop_cmd.empty()) {
                std::string output;
                utils::ProcessManager::run_command(pre_stop_cmd, output, 5);
            }
        } catch (...) {}
    }

    ProcessHandle handle = utils::ProcessManager::start_process(
        active_platform_config_.command,
        final_args,
        "",
        true,
        false,
        env_vec
    );

    set_process_handle(handle);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (has_process_handle(handle)) {
            instance_token_map_["pid"] = std::to_string(handle.pid);
        }
    }

    if (!utils::ProcessManager::is_running(handle)) {
        throw std::runtime_error("Failed to spawn process for backend: " + descriptor_->recipe);
    }

    std::string probe_endpoint = descriptor_->health_probe.endpoint.empty() ? descriptor_->health_endpoint : descriptor_->health_probe.endpoint;
    int timeout_sec = descriptor_->health_probe.timeout_seconds > 0 ? descriptor_->health_probe.timeout_seconds : descriptor_->health_timeout_seconds;
    int poll_ms = descriptor_->health_probe.poll_interval_ms > 0 ? descriptor_->health_probe.poll_interval_ms : 100;

    bool probe_ok = perform_health_probe(probe_endpoint, descriptor_->health_probe.expected_status, timeout_sec, poll_ms);

    if (!probe_ok) {
        throw std::runtime_error("Health probe failed or timed out for backend: " + descriptor_->recipe);
    }

    start_backend_watchdog(probe_endpoint);

    auto end_time = std::chrono::steady_clock::now();
    load_duration_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = ModelState::READY;
        state_cv_.notify_all();
    }
    state_guard.success = true;
}

bool ExternalBackendServer::wait_for_ready(const std::string& endpoint, long timeout_seconds, long poll_interval_ms) {
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ == ModelState::READY) return true;
    long timeout_sec = timeout_seconds;
    if (descriptor_ && descriptor_->health_probe.timeout_seconds > 0) {
        timeout_sec = descriptor_->health_probe.timeout_seconds;
    }
    return state_cv_.wait_for(lock, std::chrono::seconds(timeout_sec), [this] {
        return state_ == ModelState::READY || state_ == ModelState::UNLOADED;
    }) && state_ == ModelState::READY;
}

bool ExternalBackendServer::perform_health_probe(const std::string& endpoint,
                                                  int expected_status,
                                                  int timeout_seconds,
                                                  int poll_interval_ms) {
    (void)expected_status;
    auto start = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(timeout_seconds);

    std::string health_url = "http://127.0.0.1:" + std::to_string(port_) + endpoint;
    LOG(INFO) << "ExternalBackendServer: Performing health probe for " << descriptor_->recipe << " on " << health_url << " (timeout: " << timeout_seconds << "s)";

    while (std::chrono::steady_clock::now() - start < timeout) {
        if (load_cancel_ && load_cancel_->load(std::memory_order_relaxed)) {
            LOG(WARNING) << "ExternalBackendServer: Health probe cancelled for " << descriptor_->recipe;
            return false;
        }

        ProcessHandle proc = get_process_handle_snapshot();
        if (has_process_handle(proc) && !utils::ProcessManager::is_running(proc)) {
            LOG(ERROR) << "ExternalBackendServer: Subprocess died unexpectedly during health probe for " << descriptor_->recipe;
            return false;
        }

        if (utils::HttpClient::is_reachable(health_url, 1, utils::HttpSecurityPolicy::TrustedLoopback)) {
            LOG(INFO) << "ExternalBackendServer: Backend " << descriptor_->recipe << " reached on " << health_url;
            return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
    }
    LOG(ERROR) << "ExternalBackendServer: Health probe timed out after " << timeout_seconds << "s for " << descriptor_->recipe;
    return false;
}

void ExternalBackendServer::unload() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_ == ModelState::UNLOADED) {
            return;
        }
        state_ = ModelState::UNLOADED;
        state_cv_.notify_all();
    }

    stop_backend_watchdog();

    std::unordered_map<std::string, std::string> token_map_snapshot;
    BackendDescriptor::PlatformConfig config_snapshot;
    RecipeOptions options_snapshot;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        token_map_snapshot = instance_token_map_;
        config_snapshot = active_platform_config_;
        options_snapshot = loaded_recipe_options_;
    }

    // Stage 1: Custom Stop Command execution
    if ((!config_snapshot.stop_command.empty() || !config_snapshot.stop_command_args.empty()) && descriptor_) {
        try {
            std::vector<std::string> resolved_stop_args = resolve_command_args(config_snapshot.stop_command_args, token_map_snapshot, options_snapshot);
            std::vector<std::string> stop_cmd_template{config_snapshot.stop_command};
            auto resolved_cmd = resolve_command_args(stop_cmd_template, token_map_snapshot, options_snapshot);
            std::string stop_cmd = resolved_cmd.empty() ? "" : utils::escape_shell_arg(resolved_cmd[0]);

            for (const auto& arg : resolved_stop_args) {
                if (!stop_cmd.empty()) stop_cmd += " ";
                stop_cmd += utils::escape_shell_arg(arg);
            }
            if (!stop_cmd.empty()) {
                std::string output;
                utils::ProcessManager::run_command(stop_cmd, output, 15);
            }
        } catch (const std::exception& e) {
            LOG(WARNING, "ExternalBackendServer") << "Custom stop command failed during unload: " << e.what() << std::endl;
        }
    }

    // Stage 2: OS Process Handle Supervision & Cleanup
    try {
        ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            utils::ProcessManager::stop_process(handle);
        }
    } catch (const std::exception& e) {
        LOG(WARNING, "ExternalBackendServer") << "Process handle cleanup failed during unload: " << e.what() << std::endl;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        state_ = ModelState::UNLOADED;
        state_cv_.notify_all();
    }
}

bool ExternalBackendServer::downsize() {
    if (!supports_downsize()) return true;
    try {
        json res = forward_request(descriptor_->downsize_endpoint, json::object());
        return !res.contains("error");
    } catch (...) {
        return false;
    }
}

void ExternalBackendServer::restore() {
    // No-op for dynamic external backends unless restore endpoint specified
}

json ExternalBackendServer::chat_completion(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("chat_completion", "/v1/chat/completions") : "/v1/chat/completions";
    return forward_request(path, request);
}

json ExternalBackendServer::completion(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("completion", "/v1/completions") : "/v1/completions";
    return forward_request(path, request);
}

json ExternalBackendServer::responses(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("responses", "/v1/responses") : "/v1/responses";
    return forward_request(path, request);
}

json ExternalBackendServer::embeddings(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("embeddings", "/v1/embeddings") : "/v1/embeddings";
    return forward_request(path, request);
}

json ExternalBackendServer::reranking(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("reranking", "/v1/rerank") : "/v1/rerank";
    return forward_request(path, request);
}

json ExternalBackendServer::audio_transcriptions(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("transcription", "/v1/audio/transcriptions") : "/v1/audio/transcriptions";
    return forward_request(path, request);
}

std::string ExternalBackendServer::get_streaming_address() {
    return "tcp://127.0.0.1:" + std::to_string(port_);
}

void ExternalBackendServer::audio_speech(const json& request, httplib::DataSink& sink) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("speech", "/v1/audio/speech") : "/v1/audio/speech";
    forward_streaming_request(path, request.dump(), sink, false);
}

json ExternalBackendServer::classify(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("classify", "/v1/classify") : "/v1/classify";
    return forward_request(path, request);
}

json ExternalBackendServer::image_generations(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("images_generations", "/v1/images/generations") : "/v1/images/generations";
    return forward_request(path, request);
}

json ExternalBackendServer::image_edits(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("images_edits", "/v1/images/edits") : "/v1/images/edits";
    return forward_request(path, request);
}

json ExternalBackendServer::image_variations(const json& request) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("images_variations", "/v1/images/variations") : "/v1/images/variations";
    return forward_request(path, request);
}

void ExternalBackendServer::audio_generations(const json& request, httplib::DataSink& sink) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("audio_generations", "/v1/audio/generations") : "/v1/audio/generations";
    forward_streaming_request(path, request.dump(), sink, false);
}

void ExternalBackendServer::model_3d_generations(const json& request, httplib::DataSink& sink) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("model_3d_generations", "/v1/models/3d/generations") : "/v1/models/3d/generations";
    forward_streaming_request(path, request.dump(), sink, false);
}

json ExternalBackendServer::get_slots() {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("slots", "/slots") : "/slots";
    return forward_get_request(path);
}

json ExternalBackendServer::slots_action(int slot_id, const std::string& action, const json& request_body) {
    std::string base_slots_path = descriptor_ ? descriptor_->get_endpoint_path("slots", "/slots") : "/slots";
    return forward_request(base_slots_path + "/" + std::to_string(slot_id) + "?action=" + action, request_body);
}

json ExternalBackendServer::tokenize(const json& request_body) {
    std::string path = descriptor_ ? descriptor_->get_endpoint_path("tokenize", "/v1/tokenize") : "/v1/tokenize";
    return forward_request(path, request_body);
}

} // namespace lemon
