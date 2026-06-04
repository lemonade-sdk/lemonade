#include "lemon/backends/docker_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>

namespace lemon {
namespace backends {

namespace {

std::mutex g_docker_mutex;
DockerRuntimeStatus g_runtime_status;

std::string trim(const std::string& value) {
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string shell_quote(const std::string& value) {
    std::string escaped = "'";
    for (char c : value) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    escaped += "'";
    return escaped;
}

bool run_docker_command(const std::vector<std::string>& args,
                        std::string& output,
                        int timeout_seconds = 120,
                        bool merge_stderr = false) {
    std::ostringstream cmd;
    cmd << "docker";
    for (const auto& arg : args) {
        cmd << ' ' << shell_quote(arg);
    }
    if (merge_stderr) {
        cmd << " 2>&1";
    }
    const int code = utils::ProcessManager::run_command(cmd.str(), output, timeout_seconds);
    return code == 0;
}

std::vector<std::string> tokenize_args(const std::string& args_str) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_single = false;
    bool in_double = false;

    for (size_t i = 0; i < args_str.size(); ++i) {
        const char c = args_str[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if ((c == ' ' || c == '\t' || c == '\n' || c == '\r') && !in_single && !in_double) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current += c;
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

bool contains_container_ready_message(const std::string& line) {
    std::string lower = line;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("ready to serve") != std::string::npos ||
           lower.find("ready to roll") != std::string::npos;
}

int parse_tensor_parallel_count(const std::string& serve_args) {
    const auto tokens = tokenize_args(serve_args);
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i] == "--tp" && i + 1 < tokens.size()) {
            try {
                return std::max(1, std::stoi(tokens[i + 1]));
            } catch (...) {
                return 1;
            }
        }
        if (tokens[i].size() > 5 && tokens[i].compare(0, 5, "--tp=") == 0) {
            try {
                return std::max(1, std::stoi(tokens[i].substr(5)));
            } catch (...) {
                return 1;
            }
        }
    }
    return 1;
}

void refresh_runtime_from_docker(DockerRuntimeStatus& status) {
    std::string output;
    if (!run_docker_command({"inspect", "-f", "{{.State.Running}}", docker_container_name()},
                            output, 30)) {
        status.running = false;
        status.container_id.clear();
        return;
    }
    status.running = (trim(output) == "true");
    if (!status.running) {
        status.container_id.clear();
        return;
    }

    output.clear();
    if (run_docker_command({"inspect", "-f", "{{.Id}}", docker_container_name()}, output, 30)) {
        status.container_id = trim(output);
    }
    status.container_name = docker_container_name();
    const int port = status.port > 0 ? status.port : sglang_default_port();
    status.port = port;
    status.base_url = sglang_base_url(port);
}

// HF_TOKEN is forwarded to SGLang containers only when set and non-empty in
// lemond's environment. An unset variable or empty/whitespace value is ignored
// so docker run does not receive `-e HF_TOKEN=`.
std::string lemonade_hf_token_or_empty() {
    const char* raw = std::getenv("HF_TOKEN");
    if (!raw) {
        return "";
    }
    return trim(raw);
}

} // namespace

bool lemonade_hf_token_configured() {
    return !lemonade_hf_token_or_empty().empty();
}

bool is_sglang_api_ready(const std::string& base_url) {
    try {
        const std::string api_base = normalize_sglang_base_url(base_url);
        auto response = utils::HttpClient::get(api_base + "/models", {}, 5);
        return response.status_code == 200;
    } catch (...) {
        return false;
    }
}

bool is_docker_available() {
#ifndef __linux__
    return false;
#else
    std::string output;
    return run_docker_command({"version", "--format", "{{.Client.Version}}"}, output, 15);
#endif
}

DockerRuntimeStatus get_docker_runtime_status() {
    std::lock_guard<std::mutex> lock(g_docker_mutex);
    refresh_runtime_from_docker(g_runtime_status);
    return g_runtime_status;
}

DockerLogSnapshot get_docker_container_log_snapshot() {
    DockerLogSnapshot snapshot;
#ifndef __linux__
    return snapshot;
#else
    std::string output;
    if (!run_docker_command({"logs", "--tail", "2000", docker_container_name()},
                            output, 30, true)) {
        return snapshot;
    }

    std::istringstream stream(output);
    std::string line;
    constexpr size_t k_recent_log_line_count = 10;
    while (std::getline(stream, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        snapshot.last_line = trimmed;
        snapshot.recent_lines.push_back(trimmed);
        if (snapshot.recent_lines.size() > k_recent_log_line_count) {
            snapshot.recent_lines.erase(snapshot.recent_lines.begin());
        }
        if (contains_container_ready_message(trimmed)) {
            snapshot.ready_to_serve = true;
        }
    }
    return snapshot;
#endif
}

SglangGpuInfo get_sglang_gpu_info(const std::string& model) {
    SglangGpuInfo info;
#ifndef __linux__
    return info;
#else
    const auto& models = sglang_model_serve_args();
    const auto model_it = models.find(model);
    if (model_it != models.end()) {
        info.count = parse_tensor_parallel_count(model_it->second);
    } else {
        info.count = 1;
    }

    std::string output;
    if (!run_docker_command({"exec", docker_container_name(), "rocm-smi", "--showproductname"},
                            output, 20, true)) {
        info.type = "AMD GPU";
        return info;
    }

    std::map<std::string, int> series_counts;
    std::istringstream stream(output);
    std::string line;
    constexpr const char* k_series_prefix = "Card Series:";
    while (std::getline(stream, line)) {
        const auto pos = line.find(k_series_prefix);
        if (pos == std::string::npos) {
            continue;
        }
        const std::string series = trim(line.substr(pos + std::strlen(k_series_prefix)));
        if (!series.empty()) {
            series_counts[series]++;
        }
    }

    if (series_counts.empty()) {
        info.type = "AMD GPU";
        return info;
    }

    std::string best_series;
    int best_count = 0;
    for (const auto& [series, count] : series_counts) {
        if (count > best_count) {
            best_series = series;
            best_count = count;
        }
    }
    info.type = best_series;
    return info;
#endif
}

DockerStartResult start_sglang_container(const std::string& image,
                                         const std::string& model,
                                         const std::string& hf_cache_dir) {
    DockerStartResult result;
#ifndef __linux__
    result.error = "Docker SGLang containers are only supported on Linux";
    return result;
#endif

    if (!is_docker_available()) {
        result.error = "Docker CLI is not available";
        return result;
    }

    const auto& images = docker_images();
    if (std::find(images.begin(), images.end(), image) == images.end()) {
        result.error = "Unsupported docker image: " + image;
        return result;
    }

    const auto& models = sglang_model_serve_args();
    const auto model_it = models.find(model);
    if (model_it == models.end()) {
        result.error = "Unsupported model: " + model;
        return result;
    }

    const int port = sglang_default_port();
    const std::string base_url = sglang_base_url(port);

    std::lock_guard<std::mutex> lock(g_docker_mutex);

    {
        std::string output;
        run_docker_command({"rm", "-f", docker_container_name()}, output, 60);
    }

    std::vector<std::string> args = {
        "run", "-d", "--rm",
        "--name", docker_container_name(),
        "--network=host",
        "--privileged",
        "--device=/dev/kfd",
        "--device=/dev/dri",
        "--ipc=host",
        "--shm-size=16G",
        "--group-add", "video",
        "--cap-add=SYS_PTRACE",
        "--security-opt", "seccomp=unconfined",
    };

    if (const std::string hf_token = lemonade_hf_token_or_empty(); !hf_token.empty()) {
        args.push_back("-e");
        args.push_back("HF_TOKEN=" + hf_token);
    }
    args.push_back("-e");
    args.push_back("SGLANG_USE_AITER=1");
    args.push_back("-e");
    args.push_back("SGLANG_ENABLE_SPEC_V2=1");

    if (!hf_cache_dir.empty()) {
        args.push_back("-v");
        args.push_back(hf_cache_dir + ":/root/.cache/huggingface/");
    }

    args.push_back(image);
    args.push_back("python3");
    args.push_back("-m");
    args.push_back("sglang.launch_server");
    args.push_back("--model-path");
    args.push_back(model);
    args.push_back("--port");
    args.push_back(std::to_string(port));

    for (const auto& extra : tokenize_args(model_it->second)) {
        args.push_back(extra);
    }

    std::string output;
    if (!run_docker_command(args, output, 180)) {
        result.error = "Failed to start container: " + trim(output);
        return result;
    }

    result.container_id = trim(output);
    if (result.container_id.empty()) {
        result.error = "Docker did not return a container id";
        return result;
    }

    g_runtime_status.running = true;
    g_runtime_status.container_id = result.container_id;
    g_runtime_status.container_name = docker_container_name();
    g_runtime_status.image = image;
    g_runtime_status.model = model;
    g_runtime_status.base_url = base_url;
    g_runtime_status.port = port;
    g_runtime_status.error.clear();

    result.base_url = base_url;
    result.provider = docker_cloud_provider_name();
    result.model = model;
    result.port = port;
    result.ready = is_sglang_api_ready(base_url);

    LOG(INFO, "Docker") << "Started SGLang container " << result.container_id
                        << " for model " << model << " at " << base_url
                        << (result.ready ? " (ready)" : " (starting)") << std::endl;
    return result;
}

DockerRuntimeStatus stop_sglang_container() {
    DockerRuntimeStatus status;
#ifndef __linux__
    status.error = "Docker SGLang containers are only supported on Linux";
    return status;
#endif

    if (!is_docker_available()) {
        status.error = "Docker CLI is not available";
        return status;
    }

    std::lock_guard<std::mutex> lock(g_docker_mutex);

    std::string output;
    if (!run_docker_command({"rm", "-f", docker_container_name()}, output, 120)) {
        refresh_runtime_from_docker(g_runtime_status);
        if (g_runtime_status.running) {
            status.error = "Failed to stop container: " + trim(output);
            return status;
        }
    }

    g_runtime_status = DockerRuntimeStatus{};
    status.running = false;
    LOG(INFO, "Docker") << "Stopped SGLang container" << std::endl;
    return status;
}

} // namespace backends
} // namespace lemon
