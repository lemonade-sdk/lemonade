#include "lemon_cli/lemonade_client.h"
#include "lemon_cli/model_selection.h"
#include "lemon_cli/recipe_import.h"
#include <lemon/recipe_options.h>
#include <lemon/version.h>
#include <lemon_cli/agent_launcher.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/network_beacon.h>
#include <lemon/utils/custom_args.h>
#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <functional>
#include <map>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <shellapi.h>
    typedef int socklen_t;
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/wait.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

#include "lemon/utils/aixlog.hpp"

static const std::vector<std::string> VALID_LABELS = {
    "coding",
    "embeddings",
    "hot",
    "reasoning",
    "reranking",
    "tool-calling",
    "vision"
};

static const std::vector<std::string> SUPPORTED_AGENTS = {
    "claude",
    "codex"
};

static bool prompt_agent_selection(std::string& agent_out) {
    std::cout << "Select an agent to launch:" << std::endl;
    for (size_t i = 0; i < SUPPORTED_AGENTS.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << SUPPORTED_AGENTS[i] << std::endl;
    }

    std::cout << "Enter number: " << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) {
        std::cerr << "Error: Failed to read agent selection." << std::endl;
        return false;
    }

    size_t parsed_chars = 0;
    int selected = 0;
    try {
        selected = std::stoi(input, &parsed_chars);
    } catch (const std::exception&) {
        std::cerr << "Error: Invalid selection." << std::endl;
        return false;
    }

    if (parsed_chars != input.size() || selected < 1 || static_cast<size_t>(selected) > SUPPORTED_AGENTS.size()) {
        std::cerr << "Error: Selection out of range." << std::endl;
        return false;
    }

    agent_out = SUPPORTED_AGENTS[static_cast<size_t>(selected - 1)];
    std::cout << "Selected agent: " << agent_out << std::endl;
    return true;
}

// Configuration structure for CLI options
struct CliConfig {
    std::string host = "127.0.0.1";
    int port = 13305;
    std::string api_key;
    std::string model;
    std::map<std::string, std::string> checkpoints;
    std::string recipe;
    std::vector<std::string> labels;
    nlohmann::json recipe_options;
    bool save_options = false;
    std::string backend_spec;  // Format: "recipe:backend"
    bool force = false;
    std::string output_file;
    bool downloaded = false;
    std::string agent;
    std::string repo_dir;
    std::string recipe_file;
    bool skip_prompt = false;
    bool yes = false;
    int scan_duration = 30;
    bool json_output = false;
    bool codex_use_user_config = false;
    std::string codex_model_provider = "lemonade";
    std::string agent_args;
};

// Open a URL via the OS without invoking a shell (avoids shell injection).
// On Windows, ShellExecuteA is already shell-free.
// On macOS/Linux, we fork+execvp the opener directly.
#ifndef _WIN32
static int exec_open_url(const char* opener, const std::string& url, bool wait) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        // Child: redirect stdout/stderr to /dev/null
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp(opener, opener, url.c_str(), nullptr);
        _exit(127);  // execlp failed
    }
    if (wait) {
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    return 0;  // fire-and-forget
}
#endif

// Try to open a lemonade:// URL via the OS. Returns true if the OS reports success.
static bool try_lemonade_protocol(const std::string& lemonade_url) {
#ifdef _WIN32
    // Check registry before calling ShellExecuteA — Windows shows a "Get an app"
    // dialog for unregistered URI schemes and still returns > 32 (success).
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, "lemonade", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(hKey);
    HINSTANCE result = ShellExecuteA(nullptr, "open", lemonade_url.c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(result) > 32;
#elif defined(__APPLE__)
    return exec_open_url("open", lemonade_url, true) == 0;
#else
    return exec_open_url("xdg-open", lemonade_url, true) == 0;
#endif
}

static void open_url(const std::string& host, int port, const std::string& path = "/") {
    // Map web path to lemonade:// route and try the desktop app first
    std::string lemonade_url = "lemonade://open";
    if (path == "/?logs=true") {
        lemonade_url = "lemonade://open?view=logs";
    }

    if (try_lemonade_protocol(lemonade_url)) {
        return;  // Desktop app handled it
    }

    // Fall back to web app in browser
    std::string url = "http://" + host + ":" + std::to_string(port) + path;
    std::cout << "Opening URL: " << url << std::endl;

#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    int result = 0;
#elif defined(__APPLE__)
    int result = exec_open_url("open", url, false);
#else
    int result = exec_open_url("xdg-open", url, false);
#endif

    if (result != 0) {
        std::cerr << "Couldn't launch browser. Open the URL above manually" << std::endl;
        std::cout << url << std::endl;
    }
}

static bool handle_backend_operation(const std::string& spec, const std::string& operation_name,
                                    std::function<int(const std::string&, const std::string&)> action) {
    if (spec.empty()) {
        return false;
    }
    size_t colon_pos = spec.find(':');
    if (colon_pos == std::string::npos) {
        std::cerr << "Error: " << operation_name << " requires recipe:backend format (e.g., llamacpp:vulkan)" << std::endl;
        return true;
    }
    std::string recipe_name = spec.substr(0, colon_pos);
    std::string backend_name = spec.substr(colon_pos + 1);
    action(recipe_name, backend_name);
    return true;
}

static int handle_import_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    if (!config.model.empty()) {
        return lemon_cli::import_model_from_json_file(client, config.model);
    }

    return lemon_cli::import_remote_recipe(client, config.repo_dir, config.recipe_file,
                                           config.skip_prompt, config.yes, nullptr, true);
}

static int handle_pull_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    nlohmann::json model_data;

    // Build model_data JSON from command line options
    model_data["model_name"] = config.model;
    model_data["recipe"] = config.recipe;

    if (!config.checkpoints.empty()) {
        model_data["checkpoints"] = config.checkpoints;
    }

    if (!config.labels.empty()) {
        model_data["labels"] = config.labels;
    }

    return client.pull_model(model_data);
}

static int handle_export_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    nlohmann::json model_json = client.get_model_info(config.model);

    if (model_json.empty()) {
        std::cerr << "Error: Failed to fetch model info for '" << config.model << "'" << std::endl;
        return 1;
    }

    if (!lemon_cli::validate_and_transform_model_json(model_json)) {
        return 1;
    }

    std::string output = model_json.dump(4);

    if (config.output_file.empty()) {
        std::cout << output << std::endl;
    } else {
        std::ofstream file(config.output_file);
        if (!file.is_open()) {
            std::cerr << "Error: Failed to open output file '" << config.output_file << "'" << std::endl;
            return 1;
        }
        file << output;
        file.close();
        std::cout << "Model info exported to '" << config.output_file << "'" << std::endl;
    }

    return 0;
}

static int handle_load_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    // First, check if the model is downloaded
    nlohmann::json model_info = client.get_model_info(config.model);

    if (model_info.empty()) {
        std::cerr << "Error: Failed to fetch model info for '" << config.model << "'" << std::endl;
        return 1;
    }

    // Check if model is downloaded
    if (!model_info.contains("downloaded") || !model_info["downloaded"].is_boolean()) {
        std::cerr << "Error: Failed to determine download status for model '" << config.model << "'" << std::endl;
        return 1;
    }

    bool is_downloaded = model_info["downloaded"].get<bool>();

    if (!is_downloaded) {
        std::cout << "Model '" << config.model << "' is not downloaded. Pulling..." << std::endl;
        nlohmann::json pull_request;
        pull_request["model_name"] = config.model;
        int pull_result = client.pull_model(pull_request);
        if (pull_result != 0) {
            std::cerr << "Error: Failed to pull model '" << config.model << "'" << std::endl;
            return pull_result;
        }
        std::cout << "Model pulled successfully." << std::endl;
    }

    // Proceed with loading the model
    return client.load_model(config.model, config.recipe_options, config.save_options);
}

static int handle_run_command(lemonade::LemonadeClient& client, CliConfig& config) {
    if (!lemon_cli::resolve_model_if_missing(client, config.model, "run", true)) {
        return 1;
    }

    int load_result = handle_load_command(client, config);
    if (load_result != 0) {
        return load_result;
    }

    open_url(config.host, config.port);
    return 0;
}

static int handle_backends_command(lemonade::LemonadeClient& client,
                                   const CliConfig& config,
                                   bool install_requested,
                                   bool uninstall_requested) {
    if (install_requested) {
        int result = 0;
        handle_backend_operation(config.backend_spec, "Install",
            [&client, &config, &result](const std::string& recipe, const std::string& backend) {
                result = client.install_backend(recipe, backend, config.force);
                return result;
            });
        return result;
    }

    if (uninstall_requested) {
        int result = 0;
        handle_backend_operation(config.backend_spec, "Uninstall",
            [&client, &result](const std::string& recipe, const std::string& backend) {
                result = client.uninstall_backend(recipe, backend);
                return result;
            });
        return result;
    }

    return client.list_recipes();
}

static int handle_launch_command(lemonade::LemonadeClient& client, CliConfig& config) {
    if (config.agent.empty() && !prompt_agent_selection(config.agent)) {
        return 1;
    }

    const bool model_was_missing = config.model.empty();
    if (!lemon_cli::resolve_model_if_missing(client, config.model, "launch", true, config.agent)) {
        return 1;
    }

    if (model_was_missing) {
        // Interactive model resolution for launch already handled recipe selection/import choices.
    } else {
        std::cout << "Model was provided explicitly; skipping recipe import prompts." << std::endl;
    }

    lemon_tray::AgentConfig agent_config;
    lemon_tray::AgentLaunchOptions launch_options;
    std::string config_error;

    if (config.codex_use_user_config) {
        if (config.agent != "codex") {
            LOG(ERROR, "AgentBuilder") << "--provider is only supported for the codex agent." << std::endl;
            return 1;
        }
    }

    launch_options.codex_use_user_config = config.codex_use_user_config;
    launch_options.codex_model_provider = config.codex_model_provider;

    // Build agent config
    if (!lemon_tray::build_agent_config(config.agent, config.host, config.port, config.model,
                                         config.api_key, launch_options,
                                         agent_config, config_error)) {
        LOG(ERROR, "AgentBuilder") << "Failed to build agent config: " << config_error << std::endl;
        return 1;
    }

    if (config.api_key.empty()) {
        std::cout << "Launch auth: no API key provided; using default agent auth token." << std::endl;
    } else {
        std::cout << "Launch auth: API key provided and propagated to the launched agent." << std::endl;
    }

    if (!config.agent_args.empty()) {
        std::vector<std::string> user_args = lemon::utils::parse_custom_args(config.agent_args);
        agent_config.extra_args.insert(agent_config.extra_args.end(), user_args.begin(), user_args.end());
    }

    // Find agent binary
    const std::string agent_binary = lemon_tray::find_agent_binary(agent_config);
    if (agent_binary.empty()) {
        LOG(ERROR, "AgentBuilder") << "Agent binary not found for " << config.agent << std::endl;
        if (!agent_config.install_instructions.empty()) {
            LOG(ERROR, "AgentBuilder") << agent_config.install_instructions << std::endl;
        }
        return 1;
    }

    std::cout << "Loading model in background: " << config.model << std::endl;

    // Trigger load asynchronously so launch is non-blocking for agent startup.
    std::thread([host = config.host,
                 port = config.port,
                 api_key = config.api_key,
                 model = config.model,
                 recipe_options = config.recipe_options]() {
        try {
            lemonade::LemonadeClient async_client(host, port, api_key);
            nlohmann::json request_body = recipe_options;
            request_body["model_name"] = model;
            request_body["save_options"] = false;
            // Keep async load silent to avoid disrupting interactive agent UIs.
            (void)async_client.make_request("/api/v1/load", "POST", request_body.dump(), "application/json");
        } catch (const std::exception& e) {
            (void)e;
        }
    }).detach();

    std::cout << "Launching " << config.agent << "..." << std::endl;

    // Launch agent process
    lemon::utils::ProcessHandle handle;
    try {
        handle = lemon::utils::ProcessManager::start_process(
            agent_binary,
            agent_config.extra_args,
            "",
            true,
            false,
            agent_config.env_vars);
    } catch (const std::exception& e) {
        LOG(ERROR, "AgentLauncher") << "Error: Failed to launch agent process: " << e.what() << std::endl;
        return 1;
    }

    return lemon::utils::ProcessManager::wait_for_exit(handle, -1);
}

// Attempt a quick liveness check against the given host:port
static bool try_live_check(const std::string& host, int port, const std::string& api_key,
                           int timeout_ms = 500) {
    try {
        lemonade::LemonadeClient client(host, port, api_key);
        client.make_request("/live", "GET", "", "", timeout_ms, timeout_ms);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// RAII wrapper for a UDP socket bound to the beacon port, used by both
// discover_local_server_port() and handle_scan_command().
struct BeaconListener {
#ifdef _WIN32
    SOCKET fd = INVALID_SOCKET;
    bool wsa_initialized = false;
#else
    int fd = -1;
#endif
    bool valid = false;

    BeaconListener(int beacon_port, int recv_timeout_ms) {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;
        wsa_initialized = true;
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd == INVALID_SOCKET) return;
#else
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return;
#endif

        int enable_broadcast = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (char*)&enable_broadcast, sizeof(enable_broadcast));

        int reuse_addr = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse_addr, sizeof(reuse_addr));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(beacon_port);

        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) return;

        struct timeval timeout;
        timeout.tv_sec = recv_timeout_ms / 1000;
        timeout.tv_usec = (recv_timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

        valid = true;
    }

    ~BeaconListener() {
#ifdef _WIN32
        if (fd != INVALID_SOCKET) closesocket(fd);
        if (wsa_initialized) WSACleanup();
#else
        if (fd >= 0) close(fd);
#endif
    }

    BeaconListener(const BeaconListener&) = delete;
    BeaconListener& operator=(const BeaconListener&) = delete;
};

// Listen for a UDP beacon from localhost and return the server's HTTP port, or 0 if none found
static int discover_local_server_port() {
    BeaconListener listener(13305, 250);
    if (!listener.valid) return 0;

    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 3) {
            break;
        }

        char buffer[1024];
        sockaddr_in client_addr{};
        socklen_t addr_size = sizeof(client_addr);

        int bytes_received = recvfrom(listener.fd, buffer, sizeof(buffer) - 1, 0,
                                       (sockaddr*)&client_addr, &addr_size);

        if (bytes_received <= 0) {
            continue;
        }

        // Only accept beacons from localhost
        if (client_addr.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
            continue;
        }

        buffer[bytes_received] = '\0';

        try {
            nlohmann::json beacon_data = nlohmann::json::parse(buffer);

            if (beacon_data.contains("url")) {
                std::string url = beacon_data["url"].get<std::string>();

                // Extract port from URL like "http://127.0.0.1:PORT/"
                size_t colon_pos = url.rfind(':');
                if (colon_pos != std::string::npos) {
                    size_t port_start = colon_pos + 1;
                    size_t port_end = url.find('/', port_start);
                    std::string port_str = (port_end != std::string::npos)
                        ? url.substr(port_start, port_end - port_start)
                        : url.substr(port_start);
                    try {
                        return std::stoi(port_str);
                    } catch (...) {
                        continue;
                    }
                }
            }
        } catch (const nlohmann::json::exception&) {
            // Not a valid JSON beacon, ignore
        }
    }

    return 0;
}

static std::string json_value_to_string(const nlohmann::json& val) {
    if (val.is_string()) return val.get<std::string>();
    if (val.is_boolean()) return val.get<bool>() ? "true" : "false";
    if (val.is_number_integer()) return std::to_string(val.get<int64_t>());
    if (val.is_number_float()) {
        std::ostringstream oss;
        oss << val.get<double>();
        return oss.str();
    }
    return val.dump();
}

static std::string normalize_key(std::string s) {
    std::replace(s.begin(), s.end(), '-', '_');
    return s;
}

static bool is_strict_numeric(const std::string& s, bool allow_dot) {
    if (s.empty()) return false;
    size_t start = (s[0] == '-') ? 1 : 0;
    if (start >= s.size()) return false;
    bool has_dot = false;
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] == '.' && allow_dot && !has_dot) { has_dot = true; continue; }
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static nlohmann::json parse_typed_value(const std::string& value) {
    // Strict integer: optional minus, then digits only (no hex, no scientific)
    if (is_strict_numeric(value, false)) {
        try { return std::stoi(value); } catch (...) {}
    }
    // Strict double: digits with exactly one decimal point
    if (is_strict_numeric(value, true) && value.find('.') != std::string::npos) {
        try { return std::stod(value); } catch (...) {}
    }
    if (value == "true") return true;
    if (value == "false") return false;
    return value;
}

static int handle_config_view(lemonade::LemonadeClient& client) {
    try {
        std::string response = client.make_request("/internal/config");
        auto config = nlohmann::json::parse(response);

        struct Row { std::string key; std::string value; };
        std::vector<std::pair<std::string, std::vector<Row>>> sections;
        size_t max_width = 0;

        std::vector<Row> general;
        std::vector<std::string> nested_keys;
        for (auto& [key, val] : config.items()) {
            if (key == "config_version") continue;
            if (val.is_object()) {
                nested_keys.push_back(key);
            } else {
                max_width = std::max(max_width, key.size());
                general.push_back({key, json_value_to_string(val)});
            }
        }
        if (!general.empty()) {
            sections.push_back({"General", std::move(general)});
        }

        for (const auto& section_key : nested_keys) {
            std::vector<Row> rows;
            for (auto& [field, val] : config[section_key].items()) {
                std::string dk = section_key + "." + field;
                max_width = std::max(max_width, dk.size());
                rows.push_back({dk, json_value_to_string(val)});
            }
            if (!rows.empty()) {
                sections.push_back({section_key, std::move(rows)});
            }
        }

        max_width += 4;

        std::cout << "Server Configuration" << std::endl;
        std::cout << std::string(max_width + 30, '-') << std::endl;
        for (const auto& [name, rows] : sections) {
            std::cout << std::endl;
            std::cout << "  [" << name << "]" << std::endl;
            for (const auto& row : rows) {
                std::string display_val = row.value.empty() ? "(empty)" : row.value;
                std::cout << "  " << std::left << std::setw(static_cast<int>(max_width))
                          << row.key << display_val << std::endl;
            }
        }

        std::cout << std::endl;
        std::cout << "To change a value:  lemonade config set port=9000 llamacpp.backend=rocm"
                  << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error fetching config: " << e.what() << std::endl;
        return 1;
    }
}

static int handle_config_set(lemonade::LemonadeClient& client,
                             const std::vector<std::string>& raw_args) {
    nlohmann::json updates = nlohmann::json::object();

    for (const auto& arg : raw_args) {
        size_t eq_pos = arg.find('=');
        if (eq_pos == std::string::npos || eq_pos == 0) {
            std::cerr << "Error: expected key=value, got '" << arg << "'" << std::endl;
            return 1;
        }
        std::string key = arg.substr(0, eq_pos);
        std::string value = arg.substr(eq_pos + 1);

        size_t dot_pos = key.find('.');
        if (dot_pos != std::string::npos) {
            std::string section = key.substr(0, dot_pos);
            std::string field = normalize_key(key.substr(dot_pos + 1));

            if (!updates.contains(section)) {
                updates[section] = nlohmann::json::object();
            }
            updates[section][field] = parse_typed_value(value);
        } else {
            updates[normalize_key(key)] = parse_typed_value(value);
        }
    }

    if (updates.empty()) {
        std::cerr << "Error: no key-value pairs specified" << std::endl;
        std::cerr << "Usage: lemonade config set key=value [key=value ...]" << std::endl;
        std::cerr << "Example: lemonade config set llamacpp.backend=rocm port=8123" << std::endl;
        return 1;
    }

    try {
        std::string response = client.make_request(
            "/internal/set", "POST", updates.dump(), "application/json");
        auto result = nlohmann::json::parse(response);
        if (result.contains("updated")) {
            std::cout << "Configuration updated:" << std::endl;
            std::cout << result["updated"].dump(4) << std::endl;
        } else {
            std::cout << result.dump(4) << std::endl;
        }
        return 0;
    } catch (const lemonade::HttpError& e) {
        if (e.status_code() == 400) {
            try {
                auto error = nlohmann::json::parse(e.response_body());
                if (error.contains("error") && error["error"].is_string()) {
                    std::string message = error["error"].get<std::string>();
                    const std::string prefix = "Unknown config key: '";

                    if (message.rfind(prefix, 0) == 0 && !message.empty() && message.back() == '\'') {
                        std::string key = message.substr(prefix.size(),
                                                         message.size() - prefix.size() - 1);
                        std::cerr << "Error setting config: unknown config key `" << key << "`"
                                  << std::endl;
                        std::cerr << "Run `lemonade config` to see valid keys." << std::endl;
                        return 1;
                    }
                }
            } catch (const nlohmann::json::exception&) {
            }
        }

        std::cerr << "Error setting config: " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error setting config: " << e.what() << std::endl;
        return 1;
    }
}

static int handle_scan_command(const CliConfig& config) {
    const int beacon_port = 13305;
    const int scan_duration_seconds = config.scan_duration;

    std::cout << "Scanning for network beacons on port " << beacon_port << " for "
              << scan_duration_seconds << " seconds..." << std::endl;

    BeaconListener listener(beacon_port, 1000);
    if (!listener.valid) {
        std::cerr << "Error: Could not bind to beacon port " << beacon_port << std::endl;
        return 1;
    }

    // Store discovered beacons (use URL as key to avoid duplicates)
    std::unordered_set<std::string> discovered_urls;
    std::vector<std::pair<std::string, std::string>> beacon_details; // hostname, url

    std::cout << "Listening for beacons..." << std::endl;
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();

        if (elapsed_seconds >= scan_duration_seconds) {
            break;
        }

        // Receive beacon data
        char buffer[1024];
        sockaddr_in client_addr{};
        socklen_t addr_size = sizeof(client_addr);

        int bytes_received = recvfrom(listener.fd, buffer, sizeof(buffer) - 1, 0,
                                       (sockaddr*)&client_addr, &addr_size);

        if (bytes_received > 0) {
            buffer[bytes_received] = '\0';

            // Parse JSON beacon
            try {
                nlohmann::json beacon_data = nlohmann::json::parse(buffer);

                if (beacon_data.contains("service") && beacon_data.contains("hostname") &&
                    beacon_data.contains("url")) {
                    std::string hostname = beacon_data["hostname"].get<std::string>();
                    std::string url = beacon_data["url"].get<std::string>();

                    // Only add if not already discovered
                    if (discovered_urls.find(url) == discovered_urls.end()) {
                        discovered_urls.insert(url);
                        beacon_details.push_back({hostname, url});
                        std::cout << "  Discovered: " << hostname << " at " << url << std::endl;
                    }
                }
            } catch (const nlohmann::json::exception& e) {
                // Not a valid JSON beacon, ignore
                (void)e;
            }
        }
    }

    // Print summary
    std::cout << "\nScan complete. Found " << beacon_details.size() << " beacon(s):" << std::endl;

    if (beacon_details.empty()) {
        std::cout << "  No beacons discovered." << std::endl;
    } else {
        for (const auto& [hostname, url] : beacon_details) {
            std::cout << "  - " << hostname << " at " << url << std::endl;
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    // CLI11 configuration
    CLI::App app{"Lemonade CLI - HTTP client for Lemonade Server"};

    // Create config object and bind CLI11 options directly to it
    CliConfig config;

    // Set up CLI11 options with callbacks that write directly to config
    app.set_help_flag("--help,-h", "Display help information");
    app.set_help_all_flag("--help-all", "Display help information for all subcommands");
    app.set_version_flag("--version,-v", ("lemonade version " LEMON_VERSION_STRING));
    app.fallthrough(true);

    // Global options (available to all subcommands)
    auto* host_opt = app.add_option("--host", config.host, "Server host")->default_val(config.host)->type_name("HOST")->envname("LEMONADE_HOST");
    auto* port_opt = app.add_option("--port", config.port, "Server port")->default_val(config.port)->type_name("PORT")->envname("LEMONADE_PORT");
    app.add_option("--api-key", config.api_key, "API key for authentication")
        ->default_val(config.api_key)
        ->type_name("KEY")
        ->envname("LEMONADE_API_KEY");

    // Subcommands
    // Quick start commands
    CLI::App* run_cmd = app.add_subcommand("run", "Load a model and open the webapp in browser")->group("Quick start");
    CLI::App* launch_cmd = app.add_subcommand("launch", "Launch an agent with a model")->group("Quick start");

    // Server commands
    CLI::App* backends_cmd = app.add_subcommand("backends", "List available recipes and backends")->group("Server");
    backends_cmd->alias("recipes");
    CLI::App* backends_install_cmd = backends_cmd->add_subcommand("install", "Install a backend")->group("Subcommands");
    CLI::App* backends_uninstall_cmd = backends_cmd->add_subcommand("uninstall", "Uninstall a backend")->group("Subcommands");
    CLI::App* status_cmd = app.add_subcommand("status", "Check server status")->group("Server");
    status_cmd->add_flag("--json", config.json_output, "Output status as JSON");
    CLI::App* logs_cmd = app.add_subcommand("logs", "Open server logs in the web UI")->group("Server");
    CLI::App* scan_cmd = app.add_subcommand("scan", "Scan for network beacons")->group("Server");

    // Config commands
    CLI::App* config_cmd = app.add_subcommand("config", "View or modify server configuration")->group("Server");
    CLI::App* config_set_cmd = config_cmd->add_subcommand("set", "Set configuration values (e.g., llamacpp.backend=rocm port=8123)")->group("Subcommands");
    config_set_cmd->allow_extras(true);
    config_set_cmd->fallthrough(false);

    // Model commands
    CLI::App* list_cmd = app.add_subcommand("list", "List available models")->group("Model management");
    CLI::App* pull_cmd = app.add_subcommand("pull", "Pull/download a model")->group("Model management");
    CLI::App* delete_cmd = app.add_subcommand("delete", "Delete a model")->group("Model management");
    CLI::App* load_cmd = app.add_subcommand("load", "Load a model")->group("Model management");
    CLI::App* unload_cmd = app.add_subcommand("unload", "Unload a model (or all models)")->group("Model management");
    CLI::App* import_cmd = app.add_subcommand("import", "Import a model from JSON file")->group("Model management");
    CLI::App* export_cmd = app.add_subcommand("export", "Export model information to JSON")->group("Model management");

    // List options
    list_cmd->add_flag("--downloaded", config.downloaded, "Save model options for future loads");

    // Backend management options
    backends_install_cmd->add_option("spec", config.backend_spec, "Backend spec (recipe:backend)")->required()->type_name("SPEC");
    backends_install_cmd->add_flag("--force", config.force, "Bypass hardware filtering when installing a backend");
    backends_uninstall_cmd->add_option("spec", config.backend_spec, "Backend spec (recipe:backend)")->required()->type_name("SPEC");

    // Pull options
    pull_cmd->add_option("model", config.model, "Model name to pull")->required()->type_name("MODEL");
    pull_cmd->add_option("--checkpoint", config.checkpoints, "Model checkpoint path")
        ->type_name("TYPE CHECKPOINT")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    pull_cmd->add_option("--recipe", config.recipe, "Model recipe (e.g., llamacpp, flm, sd-cpp, whispercpp)")
        ->type_name("RECIPE")
        ->default_val(config.recipe);
    pull_cmd->add_option("--label", config.labels, "Add label to model")
        ->type_name("LABEL")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
        ->check(CLI::IsMember(VALID_LABELS));

    // Import options
    import_cmd->add_option("json_file", config.model, "Path to JSON file")->type_name("JSON_FILE");
    import_cmd->add_option("--directory", config.repo_dir,
        "Remote recipe directory to query (e.g., coding-agents)")->type_name("DIR");
    import_cmd->add_option("--recipe-file", config.recipe_file,
        "Remote recipe JSON filename to import from the selected directory")->type_name("FILE");
    import_cmd->add_flag("--skip-prompt", config.skip_prompt,
        "Run non-interactively (requires --directory and --recipe-file for remote import)");
    import_cmd->add_flag("--yes", config.yes,
        "Alias for --skip-prompt to support non-interactive scripting");

    // Delete options
    delete_cmd->add_option("model", config.model, "Model name to delete")->required()->type_name("MODEL");

    // Load options
    load_cmd->add_option("model", config.model, "Model name to load")->required()->type_name("MODEL");
    lemon::RecipeOptions::add_cli_options(*load_cmd, config.recipe_options);
    load_cmd->add_flag("--save-options", config.save_options, "Save model options for future loads");

    // Run options (same as load)
    run_cmd->add_option("model", config.model, "Model name to run")->type_name("MODEL");
    lemon::RecipeOptions::add_cli_options(*run_cmd, config.recipe_options);
    run_cmd->add_flag("--save-options", config.save_options, "Save model options for future runs");

    // Unload options
    unload_cmd->add_option("model", config.model, "Model name to unload")->type_name("MODEL");

    // Export options
    export_cmd->add_option("model", config.model, "Model name to export")->type_name("MODEL")->required();
    export_cmd->add_option("--output", config.output_file, "Output file path (prints to stdout if not specified)")->type_name("PATH");

    // Launch options
    CLI::Option* provider_opt = nullptr;
    launch_cmd->add_option("agent", config.agent, "Agent name to launch")
        ->type_name("AGENT")
        ->check(CLI::IsMember(SUPPORTED_AGENTS));
    launch_cmd->add_option("--model,-m", config.model, "Model name to load")->type_name("MODEL");
    launch_cmd->add_option("--directory", config.repo_dir,
        "Remote recipe directory used only if you choose recipe import at prompt")
        ->type_name("DIR");
    launch_cmd->add_option("--recipe-file", config.recipe_file,
        "Remote recipe JSON filename used only if you choose recipe import at prompt")->type_name("FILE");
    provider_opt = launch_cmd->add_option("--provider,-p", config.codex_model_provider,
        "Use model provider name for Codex instead of Lemonade-injected provider definition")
        ->type_name("PROVIDER")
        ->default_val(config.codex_model_provider)
        ->expected(0, 1);
    launch_cmd->add_option("--agent-args", config.agent_args,
        "Custom arguments to pass directly to the launched agent process")
        ->type_name("ARGS")
        ->default_val(config.agent_args);
    lemon::RecipeOptions::add_cli_options(*launch_cmd, config.recipe_options);

    // Scan options
    scan_cmd->add_option("--duration", config.scan_duration, "Scan duration in seconds")->default_val(config.scan_duration)->type_name("SECONDS");

    // Parse arguments
    CLI11_PARSE(app, argc, argv);
    config.codex_use_user_config = (provider_opt != nullptr && provider_opt->count() > 0);

    // Auto-discover local server via UDP beacon if the default connection fails
    // Skip when: no command given, scan command, or user explicitly set --host/--port
    bool has_command = !app.get_subcommands().empty();
    bool explicit_target = (host_opt->count() > 0 || port_opt->count() > 0);
    if (has_command && scan_cmd->count() == 0 && !explicit_target) {
        // Localhost responds in <10ms; use short timeout. Remote hosts need more.
        bool is_local = (config.host.empty() || config.host == "127.0.0.1" ||
                         config.host == "localhost" || config.host == "0.0.0.0");
        int live_timeout_ms = is_local ? 100 : 3000;

        if (!try_live_check(config.host, config.port, config.api_key, live_timeout_ms)) {
            int discovered_port = discover_local_server_port();
            if (discovered_port > 0 && discovered_port != config.port) {
                config.port = discovered_port;
            }
        }
    }

    // If set, LEMONADE_ADMIN_API_KEY takes precedence over the regular API key
    const char* admin_api_key = std::getenv("LEMONADE_ADMIN_API_KEY");
    if (admin_api_key && admin_api_key[0]) {
        config.api_key = admin_api_key;
    }

    // Create client
    lemonade::LemonadeClient client(config.host, config.port, config.api_key);

    // Execute command
    if (status_cmd->count() > 0) {
        if (config.json_output) {
            // Verify the server is actually reachable before reporting its port.
            // Without this check, we'd report the default port even when no server is running,
            // which could cause callers (e.g. lemonade-server stop) to target the wrong process.
            bool reachable = try_live_check(config.host, config.port, config.api_key, 500);
            if (!reachable) {
                std::cerr << "Server is not running" << std::endl;
                return 1;
            }
            nlohmann::json out;
            out["port"] = config.port;
            std::cout << out.dump() << std::endl;
            return 0;
        }
        return client.status(config.port);
    } else if (list_cmd->count() > 0) {
        return client.list_models(!config.downloaded);
    } else if (pull_cmd->count() > 0) {
        return handle_pull_command(client, config);
    } else if (import_cmd->count() > 0) {
        return handle_import_command(client, config);
    } else if (delete_cmd->count() > 0) {
        return client.delete_model(config.model);
    } else if (run_cmd->count() > 0) {
        return handle_run_command(client, config);
    } else if (load_cmd->count() > 0) {
        return handle_load_command(client, config);
    } else if (unload_cmd->count() > 0) {
        return client.unload_model(config.model);
    } else if (export_cmd->count() > 0) {
        return handle_export_command(client, config);
    } else if (backends_cmd->count() > 0) {
        return handle_backends_command(client, config,
                                       backends_install_cmd->count() > 0,
                                       backends_uninstall_cmd->count() > 0);
    } else if (launch_cmd->count() > 0) {
        return handle_launch_command(client, config);
    } else if (logs_cmd->count() > 0) {
        open_url(config.host, config.port, "/?logs=true");
        return 0;
    } else if (scan_cmd->count() > 0) {
        return handle_scan_command(config);
    } else if (config_cmd->count() > 0) {
        if (config_set_cmd->count() > 0) {
            return handle_config_set(client, config_set_cmd->remaining());
        }
        return handle_config_view(client);
    } else {
        std::cerr << "Error: No command specified" << std::endl;
        std::cerr << app.help() << std::endl;
        return 1;
    }
}
