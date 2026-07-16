#include "lemon_cli/lemonade_client.h"
#include "lemon_cli/model_selection.h"
#include "lemon_cli/recipe_import.h"
#include "lemon_cli/hf_pull.h"
#include "lemon_cli/bench.h"
#include "lemon_cli/chat_repl.h"
#include <lemon_cli/agent_config_file.h>
#include <lemon/model_types.h>
#include <lemon/recipe_options.h>
#include <lemon/version.h>
#include <lemon_cli/agent_launcher.h>
#include <lemon_cli/opencode_profile.h>
#include <lemon_cli/pi_profile.h>
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
#include <optional>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <shellapi.h>
    #include <io.h>
    #include <windows.h>
    typedef int socklen_t;
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/wait.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <termios.h>
#endif

#include "lemon/utils/aixlog.hpp"

static const std::vector<std::string> VALID_LABELS = {
    "coding",
    "embeddings",
    "hot",
    "mtp",
    "reasoning",
    "reranking",
    "tool-calling",
    "vision"
};

static const std::vector<std::string> SUPPORTED_AGENTS = {
    "claude",
    "codex",
    "opencode",
    "pi"
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

static bool is_supported_agent_name(const std::string& token) {
    return std::find(SUPPORTED_AGENTS.begin(), SUPPORTED_AGENTS.end(), token) != SUPPORTED_AGENTS.end();
}

static bool is_launch_provider_misuse(int argc, char* argv[]) {
    bool saw_launch = false;
    std::string launch_agent;

    for (int i = 1; i < argc; ++i) {
        const std::string token = argv[i];
        if (!saw_launch) {
            if (token == "launch") {
                saw_launch = true;
            }
            continue;
        }

        if (token == "--provider" || token == "-p" ||
            token.rfind("--provider=", 0) == 0 || token.rfind("-p=", 0) == 0) {
            return launch_agent != "codex";
        }

        if (!token.empty() && token[0] != '-' && launch_agent.empty() && is_supported_agent_name(token)) {
            launch_agent = token;
        }
    }

    return false;
}

static void hide_all_options_except_help(CLI::App& app) {
    for (auto* opt : app.get_options()) {
        if (opt != nullptr) {
            const std::string name = opt->get_name();
            if (name != "--help" && name != "--help-all" && name != "-h") {
                opt->group("");
            }
        }
    }
}

// Configuration structure for CLI options
struct CliConfig {
    std::string host = "127.0.0.1";
    int port = 13305;
    std::string api_key;
    std::string model;
    std::string list_filter;
    std::map<std::string, std::string> checkpoints;
    std::string recipe;
    std::string model_source = "huggingface";
    bool model_source_explicit = false;
    std::vector<std::string> labels;
    std::vector<std::string> components;
    nlohmann::json recipe_options;
    bool save_options = false;
    std::optional<bool> pinned = std::nullopt;
    std::string backend_spec;  // Format: "recipe:backend"
    bool backends_showall = false;
    bool force = false;
    std::string output_file;
    bool downloaded = false;
    bool dry_run = false;
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

    // Cloud provider commands
    std::string cloud_provider;
    std::string cloud_base_url;
    std::string cloud_api_key;
    bool cloud_allow_insecure_http = false;

    // Telemetry toggle options
    std::string telemetry_status;

    // Chat REPL options
    bool chat_cli = false;
    bool chat_no_stream = false;
    std::string chat_system_prompt;

    // Bench command options
    lemon_cli::BenchCliOptions bench;
};

// Read a line from stdin with terminal echo disabled, so secrets (API keys,
// passwords) don't linger in scrollback / screen-share. Returns the typed
// line (without trailing newline). Falls back to plain getline if the
// terminal can't be put into no-echo mode — better to keep the prompt
// usable than to refuse it.
static std::string read_secret_line() {
    std::string out;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    bool restored = false;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode & ~ENABLE_ECHO_INPUT);
        restored = true;
    }
    std::getline(std::cin, out);
    if (restored) {
        SetConsoleMode(h, mode);
    }
#else
    struct termios old_tio{}, new_tio{};
    bool restored = false;
    if (tcgetattr(fileno(stdin), &old_tio) == 0) {
        new_tio = old_tio;
        new_tio.c_lflag &= ~ECHO;
        if (tcsetattr(fileno(stdin), TCSANOW, &new_tio) == 0) {
            restored = true;
        }
    }
    std::getline(std::cin, out);
    if (restored) {
        tcsetattr(fileno(stdin), TCSANOW, &old_tio);
    }
#endif
    // Echo a newline so the cursor advances — the user's Enter was swallowed
    // along with the rest of the input when echo was off.
    std::cout << std::endl;
    return out;
}

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

static std::optional<std::string> explicit_registry_source_from_url(const std::string& value) {
    if (value.rfind("https://huggingface.co/", 0) == 0 ||
        value.rfind("http://huggingface.co/", 0) == 0) {
        return "huggingface";
    }
    for (const char* prefix : {
             "https://modelscope.cn/models/",
             "https://www.modelscope.cn/models/",
             "http://modelscope.cn/models/",
             "http://www.modelscope.cn/models/",
             "https://modelscope.ai/models/",
             "https://www.modelscope.ai/models/"}) {
        if (value.rfind(prefix, 0) == 0) return "modelscope";
    }
    return std::nullopt;
}

static int handle_manual_pull_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    nlohmann::json model_data;

    // Build model_data JSON from command line options
    model_data["model_name"] = config.model;
    model_data["recipe"] = config.recipe;

    std::optional<std::string> explicit_source;
    if (!config.checkpoints.empty()) {
        nlohmann::json checkpoints = nlohmann::json::object();
        for (const auto& [type, checkpoint] : config.checkpoints) {
            std::string detected_source;
            checkpoints[type] = lemon_cli::normalize_registry_checkpoint_arg(
                checkpoint, config.model_source, &detected_source);

            if (auto source_from_url = explicit_registry_source_from_url(checkpoint)) {
                if (config.model_source_explicit &&
                    config.model_source != *source_from_url) {
                    std::cerr
                        << "Error: checkpoint URL uses " << *source_from_url
                        << " but --source was set to " << config.model_source
                        << "." << std::endl;
                    return 1;
                }

                if (explicit_source && *explicit_source != *source_from_url) {
                    std::cerr << "Error: all checkpoints in one model must use the same "
                                 "remote registry." << std::endl;
                    return 1;
                }
                explicit_source = *source_from_url;
            }
        }
        model_data["checkpoints"] = std::move(checkpoints);
    }

    model_data["source"] = explicit_source.value_or(config.model_source);

    if (!config.components.empty()) {
        model_data["components"] = config.components;
    }

    if (!config.labels.empty()) {
        model_data["labels"] = config.labels;
    }

    // Explicit `lemonade pull`: opt into the configured registry update check.
    return client.pull_model(model_data, "", /*upgrade=*/true);
}

static bool has_manual_pull_options(const CliConfig& config) {
    return !config.checkpoints.empty() || !config.recipe.empty() ||
           !config.labels.empty() || !config.components.empty();
}

static int handle_pull_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    if (has_manual_pull_options(config)) {
        if (lemon::is_omni_collection_recipe(config.recipe)) {
            if (config.components.empty()) {
                std::cerr << "Error: omni pull requires --components MODEL [MODEL ...]."
                          << std::endl;
                std::cerr << "       See 'lemonade pull --help'." << std::endl;
                return 1;
            }
            return handle_manual_pull_command(client, config);
        }
        if (config.checkpoints.empty()) {
            std::cerr << "Error: manual pull requires at least one --checkpoint TYPE CHECKPOINT."
                      << std::endl;
            std::cerr << "       See 'lemonade pull --help'." << std::endl;
            return 1;
        }
        if (config.recipe.empty()) {
            std::cerr << "Error: manual pull requires --recipe." << std::endl;
            std::cerr << "       See 'lemonade pull --help'." << std::endl;
            return 1;
        }
        return handle_manual_pull_command(client, config);
    }

    std::string detected_source;
    std::string normalized_model = lemon_cli::normalize_registry_checkpoint_arg(
        config.model, config.model_source, &detected_source);

    // Registry checkpoints use the interactive discovery flow; registered model
    // names remain source-independent because their persisted provenance wins.
    if (normalized_model.find('/') != std::string::npos) {
        return lemon_cli::registry_pull_flow(
            client, normalized_model, false, detected_source);
    }

    nlohmann::json model_data;
    model_data["model_name"] = config.model;
    // Explicit `lemonade pull`: opt into the configured registry update check.
    return client.pull_model(model_data, "", /*upgrade=*/true);
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

    // Pre-emptive capacity check
    std::vector<std::string> labels;
    if (model_info.contains("labels") && model_info["labels"].is_array()) {
        for (const auto& label : model_info["labels"]) {
            labels.push_back(label.get<std::string>());
        }
    }
    lemon::ModelType model_type = lemon::get_model_type_from_labels(labels);
    std::string type_str = lemon::model_type_to_string(model_type);

    try {
        std::string health_str = client.make_request("/api/v1/health", "GET", "", "", 2000, 2000);
        auto health_json = nlohmann::json::parse(health_str);
        if (health_json.contains("max_models") && health_json.contains("pinned_models")) {
            auto max_models = health_json["max_models"];
            auto pinned_models = health_json["pinned_models"];
            int max = max_models.value(type_str, -1);
            int pinned_count = pinned_models.value(type_str, 0);
            if (max != -1 && pinned_count >= max) {
                std::cerr << "Warning: All slots (" << max << ") for model type '" << type_str
                          << "' are occupied by pinned models. Loading will fail." << std::endl;
            }
        }
    } catch (...) {
        // Ignore errors checking health status pre-emptively
    }

    bool is_downloaded = false;
    if (model_info.contains("downloaded") && model_info["downloaded"].is_boolean()) {
        is_downloaded = model_info["downloaded"].get<bool>();
    }

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
    return client.load_model(config.model, config.recipe_options, config.save_options, config.pinned);
}

static int handle_chat_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    lemon_cli::ChatOptions options;
    options.initial_model = config.model;
    options.system_prompt = config.chat_system_prompt;
    options.stream = !config.chat_no_stream;
    return lemon_cli::run_chat_repl(client, options);
}

static int handle_run_command(lemonade::LemonadeClient& client, CliConfig& config) {
    if (!lemon_cli::resolve_model_if_missing(client, config.model, "run", true)) {
        return 1;
    }

    int load_result = handle_load_command(client, config);
    if (load_result != 0) {
        return load_result;
    }

    if (config.chat_cli) {
        return handle_chat_command(client, config);
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

    return client.list_recipes(config.backends_showall);
}

static std::vector<lemon_cli::AgentModelEntry> fetch_llm_models_for_sync(
    lemonade::LemonadeClient& client,
    int context_window) {
    static const std::unordered_set<std::string> non_llm_labels = {
        "embeddings",
        "reranking",
        "transcription",
        "image",
        "tts",
        "upscaling",
        "edit"
    };

    std::vector<lemon_cli::AgentModelEntry> models;

    try {
        std::string response = client.make_request(
            "/api/v1/models?show_all=false", "GET", "", "", 3000, 3000);
        const nlohmann::json parsed = nlohmann::json::parse(response);

        if (!parsed.contains("data") || !parsed["data"].is_array()) {
            return models;
        }

        for (const auto& model : parsed["data"]) {
            if (!model.is_object() || !model.contains("id") || !model["id"].is_string()) {
                continue;
            }

            bool is_llm = true;
            if (model.contains("labels") && model["labels"].is_array()) {
                for (const auto& label : model["labels"]) {
                    if (label.is_string() && non_llm_labels.count(label.get<std::string>()) > 0) {
                        is_llm = false;
                        break;
                    }
                }
            }

            if (!is_llm) {
                continue;
            }

            const std::string model_id = model["id"].get<std::string>();
            models.push_back({model_id, model_id + " (local)", context_window});
        }
    } catch (const std::exception&) {
        // Non-fatal: we still include the selected model below.
    }

    return models;
}

static void sync_agent_config_for_launch(lemonade::LemonadeClient& client,
                                         const CliConfig& config) {
    constexpr int default_context_window = 40960;
    std::vector<lemon_cli::AgentModelEntry> models =
        fetch_llm_models_for_sync(client, default_context_window);

    bool selected_present = false;
    for (const auto& model : models) {
        if (model.id == config.model) {
            selected_present = true;
            break;
        }
    }

    if (!selected_present) {
        models.push_back({config.model, config.model + " (local)", default_context_window});
    }

    const lemon_cli::AgentConfigProfile* profile = nullptr;
    if (config.agent == "opencode") {
        profile = &lemon_cli::opencode_profile();
    } else if (config.agent == "pi") {
        profile = &lemon_cli::pi_profile();
    }

    if (profile == nullptr) {
        return;
    }

    const std::string config_api_key = config.api_key.empty() ? "lemonade" : config.api_key;

    const std::string base_url =
        lemon_tray::build_agent_server_base_url(config.host, config.port) + "/v1";

    std::string error_message;
    if (!lemon_cli::sync_agent_config_file(*profile,
                                           "Lemonade",
                                           base_url,
                                           config_api_key,
                                           models,
                                           error_message)) {
        std::cerr << "Warning: Failed to sync " << config.agent
                  << " config: " << error_message << std::endl;
        std::cerr << "Continuing with launch anyway..." << std::endl;
    }

    if (config.agent == "pi") {
        // Only write settings.json if pi doesn't already have a default provider/model.
        // This preserves existing user configuration while providing seamless first-time UX.
        if (!lemon_cli::pi_has_default_config()) {
            std::string settings_error;
            if (!lemon_cli::sync_pi_settings_file("Lemonade", config.model, settings_error)) {
                std::cerr << "Warning: Failed to sync pi settings: " << settings_error << std::endl;
                std::cerr << "Continuing with launch anyway..." << std::endl;
            }
        }
    }
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

    if (lemon_tray::agent_needs_config_sync(config.agent)) {
        sync_agent_config_for_launch(client, config);
    }

    lemon_tray::AgentConfig agent_config;
    lemon_tray::AgentLaunchOptions launch_options;
    std::string config_error;

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

        std::vector<std::string> path;
        size_t last_pos = 0;
        while (true) {
            size_t next_dot = key.find('.', last_pos);
            if (next_dot == std::string::npos) {
                std::string part = key.substr(last_pos);
                if (!part.empty()) {
                    path.push_back(normalize_key(part));
                }
                break;
            }
            std::string part = key.substr(last_pos, next_dot - last_pos);
            if (!part.empty()) {
                path.push_back(normalize_key(part));
            }
            last_pos = next_dot + 1;
        }

        if (path.empty()) {
            std::cerr << "Error: empty key in '" << arg << "'" << std::endl;
            return 1;
        }

        nlohmann::json* current = &updates;
        for (size_t i = 0; i < path.size(); ++i) {
            const std::string& k = path[i];
            if (i == path.size() - 1) {
                (*current)[k] = parse_typed_value(value);
            } else {
                if (!current->contains(k) || !(*current)[k].is_object()) {
                    (*current)[k] = nlohmann::json::object();
                }
                current = &((*current)[k]);
            }
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
        if (result.contains("message") && result["message"].is_string()) {
            std::cout << result["message"].get<std::string>() << std::endl;
        }
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

        std::cerr << "Error setting config: " << lemonade::extract_server_error_message(e)
                  << std::endl;
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
    CLI::App* chat_cmd = app.add_subcommand("chat", "Open an interactive chat REPL in the terminal")->group("Quick start");

    // Server commands
    CLI::App* backends_cmd = app.add_subcommand("backends", "List supported recipes and backends. Use --all to show all backends")->group("Server");
    backends_cmd->alias("recipes");
    backends_cmd->add_flag("--all", config.backends_showall, "Show all backends");
    CLI::App* backends_install_cmd = backends_cmd->add_subcommand("install", "Install a backend")->group("Subcommands");
    CLI::App* backends_uninstall_cmd = backends_cmd->add_subcommand("uninstall", "Uninstall a backend")->group("Subcommands");
    CLI::App* status_cmd = app.add_subcommand("status", "Check server status")->group("Server");
    status_cmd->add_flag("--json", config.json_output, "Output status as JSON");
    CLI::App* logs_cmd = app.add_subcommand("logs", "Open server logs in the web UI")->group("Server");
    CLI::App* scan_cmd = app.add_subcommand("scan", "Scan for network beacons")->group("Server");

    CLI::App* telemetry_cmd = app.add_subcommand("telemetry", "Toggle server telemetry on or off")->group("Server");
    telemetry_cmd->add_option("status", config.telemetry_status, "Telemetry status: 'on' or 'off'")
        ->required()
        ->check(CLI::IsMember({"on", "off"}));

    // Config commands
    CLI::App* config_cmd = app.add_subcommand("config", "View or modify server configuration")->group("Server");
    CLI::App* config_set_cmd = config_cmd->add_subcommand("set", "Set configuration values (e.g., llamacpp.backend=rocm port=8123)")->group("Subcommands");
    config_set_cmd->allow_extras(true);
    config_set_cmd->fallthrough(false);

    // Model commands
    CLI::App* list_cmd = app.add_subcommand("list", "List available models. Use --downloaded to show only local models.")->group("Model management");
    CLI::App* check_updates_cmd = app.add_subcommand(
        "check-updates", "Check downloaded models for upstream updates")->group("Model management");
    CLI::App* pull_cmd = app.add_subcommand("pull",
        "Pull/download a model by registered name or remote registry checkpoint")->group("Model management");
    CLI::App* delete_cmd = app.add_subcommand("delete", "Delete a model")->group("Model management");
    CLI::App* load_cmd = app.add_subcommand("load", "Load a model")->group("Model management");
    CLI::App* unload_cmd = app.add_subcommand("unload", "Unload a model (or all models)")->group("Model management");
    CLI::App* pin_cmd = app.add_subcommand("pin", "Pin a loaded model to prevent eviction")->group("Model management");
    CLI::App* unpin_cmd = app.add_subcommand("unpin", "Unpin a loaded model")->group("Model management");
    CLI::App* import_cmd = app.add_subcommand("import", "Import a model from JSON file")->group("Model management");
    CLI::App* export_cmd = app.add_subcommand("export", "Export model information to JSON")->group("Model management");
    CLI::App* cleanup_cmd = app.add_subcommand("cleanup-cache", "Clean up orphaned files in the model hub cache")->group("Model management");

    // List options
    list_cmd->add_flag("--downloaded", config.downloaded, "Show only downloaded models");
    list_cmd->add_option("name_filter", config.list_filter,
        "Optional case-insensitive model-name filter; supports * wildcards")
        ->type_name("NAME_FILTER");

    // Backend management options
    backends_install_cmd->add_option("spec", config.backend_spec, "Backend spec (recipe:backend)")->required()->type_name("SPEC");
    backends_install_cmd->add_flag("--force", config.force, "Bypass hardware filtering when installing a backend");
    backends_uninstall_cmd->add_option("spec", config.backend_spec, "Backend spec (recipe:backend)")->required()->type_name("SPEC");

    // Cloud provider commands. `cloud` is a subcommand group with install /
    // uninstall / auth / list. Mirrors the `backends` group shape on purpose
    // so muscle memory transfers.
    CLI::App* cloud_cmd = app.add_subcommand("cloud", "Manage cloud OpenAI-compatible providers")->group("Server");
    CLI::App* cloud_install_cmd = cloud_cmd->add_subcommand("install", "Register a cloud provider")->group("Subcommands");
    cloud_install_cmd->add_option("provider", config.cloud_provider, "Provider name (e.g. fireworks, openai)")->required()->type_name("PROVIDER");
    cloud_install_cmd->add_option("--base-url", config.cloud_base_url, "OpenAI-compat base URL (must include /v1)")->required()->type_name("URL");
    cloud_install_cmd->add_option("--api-key", config.cloud_api_key,
        "Optional: store this key in process memory. Prefer setting LEMONADE_<PROVIDER>_API_KEY instead.")
        ->type_name("KEY");
    cloud_install_cmd->add_flag("--allow-insecure-http", config.cloud_allow_insecure_http,
        "Explicitly allow sending this provider's API key over http://.");

    CLI::App* cloud_uninstall_cmd = cloud_cmd->add_subcommand("uninstall", "Remove a cloud provider")->group("Subcommands");
    cloud_uninstall_cmd->add_option("provider", config.cloud_provider, "Provider name")->required()->type_name("PROVIDER");

    CLI::App* cloud_auth_cmd = cloud_cmd->add_subcommand("auth", "Set a runtime API key (in-memory only)")->group("Subcommands");
    cloud_auth_cmd->add_option("provider", config.cloud_provider, "Provider name")->required()->type_name("PROVIDER");
    cloud_auth_cmd->add_option("--api-key", config.cloud_api_key,
        "API key. If omitted you'll be prompted (TTY only).")
        ->type_name("KEY");
    cloud_auth_cmd->add_flag("--allow-insecure-http", config.cloud_allow_insecure_http,
        "Explicitly allow sending this provider's API key over http://.");

    CLI::App* cloud_clear_cmd = cloud_cmd->add_subcommand("clear", "Clear the runtime API key (env var unaffected)")->group("Subcommands");
    cloud_clear_cmd->add_option("provider", config.cloud_provider, "Provider name")->required()->type_name("PROVIDER");

    CLI::App* cloud_list_cmd = cloud_cmd->add_subcommand("list", "List installed cloud providers")->group("Subcommands");

    // Pull options
    pull_cmd->add_option("model", config.model,
        "Registered model name, registry checkpoint (owner/repo[:variant]), or model URL")
        ->required()
        ->type_name("MODEL_OR_CHECKPOINT");
    CLI::Option* pull_source_opt =
        pull_cmd->add_option("--source", config.model_source,
            "Remote registry for checkpoint pulls: huggingface or modelscope")
            ->type_name("SOURCE")
            ->check(CLI::IsMember({"huggingface", "modelscope"}));
    pull_cmd->add_option("--checkpoint", config.checkpoints,
        "Add a TYPE CHECKPOINT pair for a custom user.* model. Repeat for multi-file models.")
        ->group("Manual Configuration Options")
        ->type_name("TYPE CHECKPOINT")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    pull_cmd->add_option("--recipe", config.recipe,
        "Recipe for the custom user.* model (e.g., llamacpp, flm, sd-cpp, whispercpp, collection.omni, collection.router)")
        ->group("Manual Configuration Options")
        ->type_name("RECIPE")
        ->default_val(config.recipe);
    pull_cmd->add_option("--label", config.labels, "Add a label to the custom user.* model")
        ->group("Manual Configuration Options")
        ->type_name("LABEL")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll)
        ->check(CLI::IsMember(VALID_LABELS));
    pull_cmd->add_option("--components", config.components,
        "Components for a user.* omni collection (use with --recipe collection.omni). "
        "Components must already be registered (built-in or previously pulled user.* models).")
        ->group("Manual Configuration Options")
        ->type_name("MODEL")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    pull_cmd->footer(
        "Manual Configuration Guide:\n"
        "  https://lemonade-server.ai/docs/guide/configuration/custom-models/");

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
    static bool load_pinned_flag = false;
    load_cmd->add_option("model", config.model, "Model name to load")->required()->type_name("MODEL");
    lemon::RecipeOptions::add_cli_options(*load_cmd, config.recipe_options);
    load_cmd->add_flag("--save-options", config.save_options, "Save model options for future loads");
    load_cmd->add_flag("--pinned", load_pinned_flag, "Pin the model to prevent auto-eviction");

    // Run options (same as load)
    static bool run_pinned_flag = false;
    run_cmd->add_option("model", config.model, "Model name to run")->type_name("MODEL");
    lemon::RecipeOptions::add_cli_options(*run_cmd, config.recipe_options);
    run_cmd->add_flag("--save-options", config.save_options, "Save model options for future runs");
    run_cmd->add_flag("--pinned", run_pinned_flag, "Pin the model to prevent auto-eviction");
    run_cmd->add_flag("--chat-cli", config.chat_cli,
        "After loading, open an interactive chat REPL in the terminal instead of the browser");

    // Chat options
    chat_cmd->add_option("model", config.model,
        "Model to chat with (optional; uses currently loaded model if omitted)")->type_name("MODEL");
    chat_cmd->add_option("--system", config.chat_system_prompt,
        "System prompt to seed the conversation")->type_name("TEXT");
    chat_cmd->add_flag("--no-stream", config.chat_no_stream,
        "Disable streaming; print the full response when it is ready");

    // Unload options
    unload_cmd->add_option("model", config.model, "Model name to unload")->type_name("MODEL");

    // Pin/unpin options
    pin_cmd->add_option("model", config.model, "Model name to pin")->required()->type_name("MODEL");
    unpin_cmd->add_option("model", config.model, "Model name to unpin")->required()->type_name("MODEL");

    // Export options
    export_cmd->add_option("model", config.model, "Model name to export")->type_name("MODEL")->required();
    export_cmd->add_option("--output", config.output_file, "Output file path (prints to stdout if not specified)")->type_name("PATH");

    // Launch options
    auto add_common_launch_options = [&config](CLI::App& cmd) {
        cmd.add_option("--model,-m", config.model, "Model name to load")->type_name("MODEL");
        cmd.add_option("--directory", config.repo_dir,
            "Remote recipe directory used only if you choose recipe import at prompt")
            ->type_name("DIR");
        cmd.add_option("--recipe-file", config.recipe_file,
            "Remote recipe JSON filename used only if you choose recipe import at prompt")->type_name("FILE");
        cmd.add_option("--agent-args", config.agent_args,
            "Custom arguments to pass directly to the launched agent process")
            ->type_name("ARGS")
            ->default_val(config.agent_args);
    };

    add_common_launch_options(*launch_cmd);
    lemon::RecipeOptions::add_cli_options(*launch_cmd, config.recipe_options);
    hide_all_options_except_help(*launch_cmd);

    CLI::Option* codex_provider_opt = nullptr;
    for (const std::string& agent_name : SUPPORTED_AGENTS) {
        CLI::App* agent_cmd =
            launch_cmd->add_subcommand(agent_name, "Launch " + agent_name + " with a model")
                ->group("Agents");
        agent_cmd->callback([&config, agent_name]() { config.agent = agent_name; });
        add_common_launch_options(*agent_cmd);

        if (agent_name == "codex") {
            codex_provider_opt = agent_cmd->add_option("--provider,-p", config.codex_model_provider,
                "Use model provider name for Codex instead of Lemonade-injected provider definition")
                ->type_name("PROVIDER")
                ->default_val(config.codex_model_provider)
                ->expected(0, 1);
        }
    }

    // Scan options
    scan_cmd->add_option("--duration", config.scan_duration, "Scan duration in seconds")->default_val(config.scan_duration)->type_name("SECONDS");

    // Cleanup cache options
    cleanup_cmd->add_flag("--dry-run", config.dry_run, "Preview what would be cleaned up without deleting");

    // Bench command
    CLI::App* bench_cmd = lemon_cli::register_bench_command(app, config.output_file, config.bench);

    // Parse arguments
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        if (is_launch_provider_misuse(argc, argv)) {
            std::cerr << "Error: --provider is only supported for 'lemonade launch codex'." << std::endl;
            return 1;
        }
        return app.exit(e);
    }

    config.model_source_explicit = pull_source_opt->count() > 0;

    if (load_cmd->count() > 0) {
        if (load_cmd->count("--pinned") > 0) {
            config.pinned = load_pinned_flag;
        } else {
            config.pinned = std::nullopt;
        }
    } else if (run_cmd->count() > 0) {
        if (run_cmd->count("--pinned") > 0) {
            config.pinned = run_pinned_flag;
        } else {
            config.pinned = std::nullopt;
        }
    }
    config.codex_use_user_config = (codex_provider_opt != nullptr && codex_provider_opt->count() > 0);

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
            // which could cause callers to target the wrong process.
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
        return client.list_models(!config.downloaded, config.list_filter);
    } else if (check_updates_cmd->count() > 0) {
        return client.check_model_updates();
    } else if (pull_cmd->count() > 0) {
        if (config.model.empty()) {
            std::cerr << "Error: 'lemonade pull' requires a model name or remote registry checkpoint." << std::endl;
            std::cerr << "       See 'lemonade pull --help'." << std::endl;
            return 1;
        }
        return handle_pull_command(client, config);
    } else if (import_cmd->count() > 0) {
        return handle_import_command(client, config);
    } else if (delete_cmd->count() > 0) {
        return client.delete_model(config.model);
    } else if (run_cmd->count() > 0) {
        return handle_run_command(client, config);
    } else if (chat_cmd->count() > 0) {
        return handle_chat_command(client, config);
    } else if (load_cmd->count() > 0) {
        return handle_load_command(client, config);
    } else if (unload_cmd->count() > 0) {
        return client.unload_model(config.model);
    } else if (pin_cmd->count() > 0) {
        return client.pin_model(config.model, true);
    } else if (unpin_cmd->count() > 0) {
        return client.pin_model(config.model, false);
    } else if (export_cmd->count() > 0) {
        return handle_export_command(client, config);
    } else if (backends_cmd->count() > 0) {
        return handle_backends_command(client, config,
                                       backends_install_cmd->count() > 0,
                                       backends_uninstall_cmd->count() > 0);
    } else if (cloud_cmd->count() > 0) {
        if (cloud_install_cmd->count() > 0) {
            return client.install_cloud_provider(config.cloud_provider,
                                                  config.cloud_base_url,
                                                  config.cloud_api_key,
                                                  config.cloud_allow_insecure_http);
        }
        if (cloud_uninstall_cmd->count() > 0) {
            return client.uninstall_cloud_provider(config.cloud_provider);
        }
        if (cloud_auth_cmd->count() > 0) {
            // Interactive prompt only when the user didn't pass --api-key and
            // stdin is a TTY. In non-interactive contexts (CI, pipes) refuse
            // rather than silently hang on getline.
            std::string key = config.cloud_api_key;
            if (key.empty()) {
#ifdef _WIN32
                bool is_tty = _isatty(_fileno(stdin)) != 0;
#else
                bool is_tty = isatty(fileno(stdin)) != 0;
#endif
                if (!is_tty) {
                    std::cerr << "Error: --api-key is required when stdin is not a TTY"
                              << std::endl;
                    return 1;
                }
                std::cout << "API key for " << config.cloud_provider
                          << " (input hidden): ";
                std::cout.flush();
                key = read_secret_line();
                if (key.empty()) {
                    std::cerr << "Error: empty API key" << std::endl;
                    return 1;
                }
            }
            return client.cloud_auth(config.cloud_provider, key,
                                     config.cloud_allow_insecure_http);
        }
        if (cloud_clear_cmd->count() > 0) {
            return client.cloud_auth_clear(config.cloud_provider);
        }
        if (cloud_list_cmd->count() > 0) {
            return client.cloud_list();
        }
        // No subcommand specified: print help.
        std::cout << cloud_cmd->help() << std::endl;
        return 0;
    } else if (launch_cmd->count() > 0) {
        return handle_launch_command(client, config);
    } else if (logs_cmd->count() > 0) {
        open_url(config.host, config.port, "/?logs=true");
        return 0;
    } else if (scan_cmd->count() > 0) {
        return handle_scan_command(config);
    } else if (telemetry_cmd->count() > 0) {
        bool enable = (config.telemetry_status == "on");
        nlohmann::json body = {{"telemetry", {{"enabled", enable}}}};
        try {
            std::string res_str = client.make_request("/internal/set", "POST", body.dump(), "application/json");
            auto json_res = nlohmann::json::parse(res_str);
            if (json_res.contains("status") && json_res["status"] == "success") {
                std::cout << "Telemetry successfully turned " << (enable ? "on" : "off") << "." << std::endl;
                return 0;
            } else {
                std::cerr << "Failed to toggle telemetry: " << res_str << std::endl;
                return 1;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error toggling telemetry: " << e.what() << std::endl;
            return 1;
        }
    } else if (config_cmd->count() > 0) {
        if (config_set_cmd->count() > 0) {
            return handle_config_set(client, config_set_cmd->remaining());
        }
        return handle_config_view(client);
    } else if (cleanup_cmd->count() > 0) {
        return client.cleanup_cache(config.dry_run);
    } else if (bench_cmd->count() > 0) {
        auto bench_config = lemon_cli::build_bench_config(config.output_file, config.bench);
        return lemon_cli::handle_bench_command(client, bench_config);
    } else {
        std::cerr << "Error: No command specified" << std::endl;
        std::cerr << app.help() << std::endl;
        return 1;
    }
}
