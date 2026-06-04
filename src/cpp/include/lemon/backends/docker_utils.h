#pragma once

#include <map>
#include <string>
#include <vector>

namespace lemon {
namespace backends {

inline const std::vector<std::string>& docker_images() {
    static const std::vector<std::string> images = {
        "lmsysorg/sglang:v0.5.12.post1-rocm720-mi35x",
    };
    return images;
}

inline const std::map<std::string, std::string>& sglang_model_serve_args() {
    static const std::map<std::string, std::string> models = {
        {"meta-llama/Llama-2-7b-chat-hf",
         " --attention-backend aiter  --chat-template llama-2"},
        {"Qwen/Qwen3.6-35B-A3B",
         " --reasoning-parser qwen3   --tool-call-parser qwen3_coder   "
         "--speculative-algorithm EAGLE   --speculative-num-steps 3   "
         "--speculative-eagle-topk 1   --speculative-num-draft-tokens 4   "
         "--mem-fraction-static 0.8  --host 0.0.0.0 --attention-backend aiter --tp 8"},
    };
    return models;
}

inline const std::string& docker_cloud_provider_name() {
    static const std::string name = "sglang";
    return name;
}

inline const std::string& docker_cloud_api_key() {
    static const std::string key = "local lemonade";
    return key;
}

inline int sglang_default_port() { return 30000; }

inline std::string sglang_base_url(int port = 0) {
    const int resolved_port = port > 0 ? port : sglang_default_port();
    return "http://127.0.0.1:" + std::to_string(resolved_port) + "/v1";
}

inline std::string normalize_sglang_base_url(const std::string& base_url, int port = 0) {
    if (base_url.empty()) {
        return sglang_base_url(port);
    }
    std::string normalized = base_url;
    while (!normalized.empty() && normalized.back() == '/') {
        normalized.pop_back();
    }
    constexpr const char* k_v1_suffix = "/v1";
    if (normalized.size() >= 3 &&
        normalized.compare(normalized.size() - 3, 3, k_v1_suffix) == 0) {
        return normalized;
    }
    return normalized + k_v1_suffix;
}

inline const std::string& default_sglang_model() {
    static const std::string model = "Qwen/Qwen3.6-35B-A3B";
    return model;
}

inline const std::string& docker_container_name() {
    static const std::string name = "lemonade-sglang";
    return name;
}

struct DockerRuntimeStatus {
    bool running = false;
    std::string container_id;
    std::string container_name;
    std::string image;
    std::string model;
    std::string base_url;
    int port = 0;
    std::string error;
};

struct DockerStartResult {
    std::string container_id;
    std::string base_url;
    std::string provider;
    std::string model;
    int port = 0;
    bool ready = false;
    std::string error;
};

struct DockerLogSnapshot {
    std::string last_line;
    std::vector<std::string> recent_lines;
    bool ready_to_serve = false;
};

struct SglangGpuInfo {
    int count = 0;
    std::string type;
};

bool is_docker_available();

bool lemonade_hf_token_configured();

bool is_sglang_api_ready(const std::string& base_url);

DockerRuntimeStatus get_docker_runtime_status();

DockerStartResult start_sglang_container(const std::string& image,
                                         const std::string& model,
                                         const std::string& hf_cache_dir);

DockerRuntimeStatus stop_sglang_container();

DockerLogSnapshot get_docker_container_log_snapshot();

SglangGpuInfo get_sglang_gpu_info(const std::string& model);

} // namespace backends
} // namespace lemon
