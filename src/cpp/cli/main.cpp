#include "lemon_cli/lemonade_client.h"
#include <lemon/recipe_options.h>
#include <lemon/version.h>
#include <lemon_cli/agent_launcher.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/network_beacon.h>
#include <CLI/CLI.hpp>
#include <httplib.h>
#include <iostream>
#include <string>
#include <fstream>
#include <cctype>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>
#include <unordered_set>

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

static const std::vector<std::string> VALID_LABELS = {
    "coding",
    "embeddings",
    "hot",
    "reasoning",
    "reranking",
    "tool-calling",
    "vision"
};

static const std::vector<std::string> KNOWN_KEYS = {
    "checkpoint",
    "checkpoints",
    "model_name",
    "image_defaults",
    "labels",
    "recipe",
    "recipe_options",
    "size"
};

static const std::vector<std::string> SUPPORTED_AGENTS = {
    "claude",
    "codex"
};

// Configuration structure for CLI options
struct CliConfig {
    std::string host = "127.0.0.1";
    int port = 8000;
    std::string api_key;
    std::string model;
    std::map<std::string, std::string> checkpoints;
    std::string recipe;
    std::vector<std::string> labels;
    nlohmann::json recipe_options;
    bool save_options = false;
    std::string install_backend;  // Format: "recipe:backend"
    std::string uninstall_backend;  // Format: "recipe:backend"
    std::string output_file;
    bool downloaded = false;
    std::string agent;
    bool use_recipe = false;
    std::string repo_dir;
    std::string recipe_file;
    bool skip_prompt = false;
    bool yes = false;
    int scan_duration = 30;
    bool json_output = false;
};

static bool validate_and_transform_model_json(nlohmann::json& model_data) {
    // Validate model_name (or id -> model_name)
    if (!model_data.contains("model_name") || !model_data["model_name"].is_string()) {
        if (model_data.contains("id") && model_data["id"].is_string()) {
            model_data["model_name"] = model_data["id"];
            model_data.erase("id");
        } else {
            std::cerr << "Error: JSON file must contain a 'model_name' string field" << std::endl;
            return false;
        }
    }

    // Prepend "user." to model_name if it doesn't already start with "user."
    std::string model_name = model_data["model_name"].get<std::string>();
    if (model_name.substr(0, 5) != "user.") {
        model_data["model_name"] = "user." + model_name;
    }

    // Validate recipe
    if (!model_data.contains("recipe") || !model_data["recipe"].is_string()) {
        std::cerr << "Error: JSON file must contain a 'recipe' string field" << std::endl;
        return false;
    }

    // Validate checkpoints or checkpoint
    bool has_checkpoints = model_data.contains("checkpoints") && model_data["checkpoints"].is_object();
    bool has_checkpoint = model_data.contains("checkpoint") && model_data["checkpoint"].is_string();
    if (!has_checkpoints && !has_checkpoint) {
        std::cerr << "Error: JSON file must contain either 'checkpoints' (object) or 'checkpoint' (string)" << std::endl;
        return false;
    }

    // If both checkpoints and checkpoint exist, remove checkpoint
    if (has_checkpoints && has_checkpoint) {
        model_data.erase("checkpoint");
    }

    // Remove unrecognized top-level keys after validation
    std::vector<std::string> keys_to_remove;
    for (auto& [key, _] : model_data.items()) {
        bool is_known = false;
        for (const auto& known_key : KNOWN_KEYS) {
            if (key == known_key) {
                is_known = true;
                break;
            }
        }
        if (!is_known) {
            keys_to_remove.push_back(key);
        }
    }

    for (const auto& key : keys_to_remove) {
        model_data.erase(key);
    }

    return true;
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

static int import_model_from_json_file(lemonade::LemonadeClient& client,
                                       const std::string& json_path,
                                       std::string* imported_model_out = nullptr) {
    nlohmann::json model_data;

    // Load JSON from file
    std::ifstream file(json_path);
    if (!file.good()) {
        std::cerr << "Error: Failed to open JSON file '" << json_path << "'" << std::endl;
        return 1;
    }

    try {
        model_data = nlohmann::json::parse(file);
        file.close();

        if (!validate_and_transform_model_json(model_data)) {
            return 1;
        }

        if (imported_model_out != nullptr && model_data.contains("model_name") && model_data["model_name"].is_string()) {
            *imported_model_out = model_data["model_name"].get<std::string>();
        }
    } catch (const nlohmann::json::exception& e) {
        std::cerr << "Error: Failed to parse JSON file '" << json_path << "': " << e.what() << std::endl;
        return 1;
    }

    return client.pull_model(model_data);
}

static bool is_json_recipe_file(const nlohmann::json& entry) {
    if (!entry.is_object()) {
        return false;
    }
    if (!entry.contains("type") || !entry["type"].is_string() || entry["type"].get<std::string>() != "file") {
        return false;
    }
    if (!entry.contains("name") || !entry["name"].is_string()) {
        return false;
    }
    const std::string name = entry["name"].get<std::string>();
    return name.size() >= 5 && name.substr(name.size() - 5) == ".json";
}

static bool fetch_github_recipe_contents(const std::string& subpath,
                                         nlohmann::json& response_out,
                                         std::string& error_out) {
    std::string api_path = "/repos/lemonade-sdk/recipes/contents";
    if (!subpath.empty()) {
        api_path += "/" + subpath;
    }

    httplib::Client cli("https://api.github.com");
    cli.set_follow_location(true);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(10);

    httplib::Headers headers = {
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", "lemonade-cli"}
    };

    auto res = cli.Get(api_path.c_str(), headers);
    if (!res) {
        error_out = "GitHub API request failed: " + httplib::to_string(res.error());
        return false;
    }
    if (res->status != 200) {
        error_out = "GitHub API request failed with status " + std::to_string(res->status);
        return false;
    }

    try {
        response_out = nlohmann::json::parse(res->body);
    } catch (const nlohmann::json::exception& e) {
        error_out = std::string("Failed to parse GitHub API JSON: ") + e.what();
        return false;
    }

    if (!response_out.is_array()) {
        error_out = "Unexpected GitHub API response shape (expected array).";
        return false;
    }
    return true;
}

static int prompt_numbered_choice(const std::string& title,
                                  const std::vector<std::string>& options,
                                  bool allow_skip,
                                  const std::string& skip_label) {
    if (options.empty()) {
        return -2;
    }

    std::cout << title << std::endl;
    if (allow_skip) {
        std::cout << "  0) " << skip_label << std::endl;
    }
    for (size_t i = 0; i < options.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << options[i] << std::endl;
    }
    std::cout << "Enter number: " << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) {
        std::cerr << "Error: Failed to read selection." << std::endl;
        return -2;
    }

    size_t parsed_chars = 0;
    int selected = 0;
    try {
        selected = std::stoi(input, &parsed_chars);
    } catch (const std::exception&) {
        std::cerr << "Error: Invalid selection." << std::endl;
        return -2;
    }

    if (parsed_chars != input.size()) {
        std::cerr << "Error: Invalid selection." << std::endl;
        return -2;
    }

    if (allow_skip && selected == 0) {
        return -1;
    }

    if (selected < 1 || static_cast<size_t>(selected) > options.size()) {
        std::cerr << "Error: Selection out of range." << std::endl;
        return -2;
    }

    return selected - 1;
}

static bool parse_https_url(const std::string& url, std::string& host_out, std::string& path_out) {
    static const std::string prefix = "https://";
    if (url.rfind(prefix, 0) != 0) {
        return false;
    }

    const std::string remainder = url.substr(prefix.size());
    size_t slash_pos = remainder.find('/');
    if (slash_pos == std::string::npos || slash_pos == 0) {
        return false;
    }

    host_out = remainder.substr(0, slash_pos);
    path_out = remainder.substr(slash_pos);
    return !host_out.empty() && !path_out.empty();
}

static bool download_recipe_to_temp_file(const std::string& download_url,
                                         std::filesystem::path& temp_file_out,
                                         std::string& error_out) {
    std::string host;
    std::string path;
    if (!parse_https_url(download_url, host, path)) {
        error_out = "Invalid recipe download URL: " + download_url;
        return false;
    }

    httplib::Client cli("https://" + host);
    cli.set_follow_location(true);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(30);

    auto res = cli.Get(path.c_str());
    if (!res) {
        error_out = "Recipe download failed: " + httplib::to_string(res.error());
        return false;
    }
    if (res->status != 200) {
        error_out = "Recipe download failed with status " + std::to_string(res->status);
        return false;
    }

    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    temp_file_out = std::filesystem::temp_directory_path() /
        ("lemonade-recipe-" + std::to_string(timestamp) + ".json");

    std::ofstream out(temp_file_out, std::ios::binary);
    if (!out.is_open()) {
        error_out = "Failed to create temp file for recipe download.";
        return false;
    }
    out.write(res->body.data(), static_cast<std::streamsize>(res->body.size()));
    out.close();

    return true;
}

static int handle_import_remote_recipe_command(lemonade::LemonadeClient& client,
                                               const CliConfig& config,
                                               std::string* imported_model_out,
                                               bool allow_skip) {
    std::string selected_dir = config.repo_dir;
    const bool non_interactive = config.skip_prompt || config.yes;

    if (non_interactive && selected_dir.empty()) {
        std::cerr << "Error: Non-interactive mode requires --repo-dir." << std::endl;
        return 1;
    }
    if (non_interactive && config.recipe_file.empty()) {
        std::cerr << "Error: Non-interactive mode requires --recipe-file." << std::endl;
        return 1;
    }

    if (selected_dir.empty()) {
        nlohmann::json top_entries;
        std::string fetch_error;
        if (!fetch_github_recipe_contents("", top_entries, fetch_error)) {
            std::cerr << "Error: " << fetch_error << std::endl;
            return 1;
        }

        std::vector<std::string> dir_names;
        for (const auto& entry : top_entries) {
            if (entry.is_object() && entry.contains("type") && entry["type"].is_string() &&
                entry["type"].get<std::string>() == "dir" &&
                entry.contains("name") && entry["name"].is_string()) {
                dir_names.push_back(entry["name"].get<std::string>());
            }
        }

        if (dir_names.empty()) {
            std::cerr << "Error: No recipe directories found in lemonade-sdk/recipes." << std::endl;
            return 1;
        }

        const int dir_idx = prompt_numbered_choice(
            "Select a recipe directory:", dir_names, allow_skip, "Continue without recipe import");
        if (dir_idx == -1) {
            std::cout << "Skipping recipe import." << std::endl;
            return 0;
        }
        if (dir_idx < 0) {
            return 1;
        }

        selected_dir = dir_names[static_cast<size_t>(dir_idx)];
    }

    nlohmann::json dir_entries;
    std::string fetch_error;
    if (!fetch_github_recipe_contents(selected_dir, dir_entries, fetch_error)) {
        std::cerr << "Error: " << fetch_error << std::endl;
        if (allow_skip) {
            std::cout << "Continuing without recipe import." << std::endl;
            return 0;
        }
        return 1;
    }

    std::vector<nlohmann::json> recipe_entries;
    std::vector<std::string> recipe_names;
    for (const auto& entry : dir_entries) {
        if (is_json_recipe_file(entry)) {
            recipe_entries.push_back(entry);
            recipe_names.push_back(entry["name"].get<std::string>());
        }
    }

    if (recipe_entries.empty()) {
        std::cerr << "Error: No JSON recipes found in directory '" << selected_dir << "'." << std::endl;
        if (allow_skip) {
            std::cout << "Continuing without recipe import." << std::endl;
            return 0;
        }
        return 1;
    }

    nlohmann::json selected_entry;
    if (!config.recipe_file.empty()) {
        bool found = false;
        for (const auto& entry : recipe_entries) {
            if (entry["name"].get<std::string>() == config.recipe_file) {
                selected_entry = entry;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Error: Recipe file '" << config.recipe_file
                      << "' not found in directory '" << selected_dir << "'." << std::endl;
            return 1;
        }
    } else {
        const int file_idx = prompt_numbered_choice(
            "Select a recipe to import:", recipe_names, allow_skip, "Continue without recipe import");
        if (file_idx == -1) {
            std::cout << "Skipping recipe import." << std::endl;
            return 0;
        }
        if (file_idx < 0) {
            return 1;
        }
        selected_entry = recipe_entries[static_cast<size_t>(file_idx)];
    }

    if (!selected_entry.contains("download_url") || !selected_entry["download_url"].is_string()) {
        std::cerr << "Error: Selected recipe does not expose a download URL." << std::endl;
        return 1;
    }

    std::filesystem::path temp_file;
    std::string download_error;
    if (!download_recipe_to_temp_file(selected_entry["download_url"].get<std::string>(), temp_file, download_error)) {
        std::cerr << "Error: " << download_error << std::endl;
        if (allow_skip) {
            std::cout << "Continuing without recipe import." << std::endl;
            return 0;
        }
        return 1;
    }

    int import_result = import_model_from_json_file(client, temp_file.string(), imported_model_out);
    std::error_code rm_ec;
    std::filesystem::remove(temp_file, rm_ec);
    if (rm_ec) {
        std::cerr << "Warning: Failed to remove temp recipe file '" << temp_file.string() << "'." << std::endl;
    }

    return import_result;
}

static int handle_import_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    if (!config.model.empty()) {
        return import_model_from_json_file(client, config.model);
    }

    return handle_import_remote_recipe_command(client, config, nullptr, true);
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

    if (!validate_and_transform_model_json(model_json)) {
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

static bool fetch_models_from_endpoint(lemonade::LemonadeClient& client, bool show_all,
                                       std::vector<lemonade::ModelInfo>& models_out) {
    try {
        std::string path = "/api/v1/models?show_all=" + std::string(show_all ? "true" : "false");
        std::string response = client.make_request(path, "GET", "", "", 3000, 3000);
        nlohmann::json json_response = nlohmann::json::parse(response);

        if (!json_response.contains("data") || !json_response["data"].is_array()) {
            return true;
        }

        for (const auto& model_item : json_response["data"]) {
            if (!model_item.is_object()) {
                continue;
            }

            lemonade::ModelInfo info;
            if (model_item.contains("id") && model_item["id"].is_string()) {
                info.id = model_item["id"].get<std::string>();
            }
            if (model_item.contains("recipe") && model_item["recipe"].is_string()) {
                info.recipe = model_item["recipe"].get<std::string>();
            }
            if (model_item.contains("downloaded") && model_item["downloaded"].is_boolean()) {
                info.downloaded = model_item["downloaded"].get<bool>();
            }

            if (!info.id.empty()) {
                models_out.push_back(info);
            }
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to query /api/v1/models: " << e.what() << std::endl;
        return false;
    }
}

static bool prompt_model_selection(lemonade::LemonadeClient& client, std::string& model_out, bool show_all) {
    std::vector<lemonade::ModelInfo> models;
    if (!fetch_models_from_endpoint(client, false, models)) {
        return false;
    }
    if (models.empty() && !fetch_models_from_endpoint(client, true, models)) {
        return false;
    }

    if (models.empty()) {
        std::cerr << "No models available on server. Try 'lemonade list' or 'lemonade pull <MODEL>'." << std::endl;
        return false;
    }

    std::cout << "Select a model:" << std::endl;
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& model = models[i];

        // show_all determines whether to display all models or only those with recipe "llamacpp"
        if (model.recipe != "llamacpp" && !show_all) {
            continue;
        }

        std::cout << "  " << (i + 1) << ") " << model.id
                  << " [" << (model.downloaded ? "downloaded" : "not-downloaded") << "]"
                  << " (" << (model.recipe.empty() ? "-" : model.recipe) << ")"
                  << std::endl;
    }
    std::cout << "Enter number: " << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) {
        std::cerr << "Error: Failed to read model selection." << std::endl;
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

    if (parsed_chars != input.size() || selected < 1 || static_cast<size_t>(selected) > models.size()) {
        std::cerr << "Error: Selection out of range." << std::endl;
        return false;
    }

    model_out = models[static_cast<size_t>(selected - 1)].id;
    std::cout << "Selected model: " << model_out << std::endl;
    return true;
}

static bool resolve_model_if_missing(lemonade::LemonadeClient& client, CliConfig& config,
                                     const std::string& command_name, bool show_all = true) {
    if (!config.model.empty()) {
        return true;
    }

    std::cout << "No model specified for '" << command_name << "'." << std::endl;
    return prompt_model_selection(client, config.model, show_all);
}

static bool prompt_yes_no(const std::string& prompt, bool default_yes = false) {
    std::cout << prompt << (default_yes ? " [Y/n]: " : " [y/N]: ") << std::flush;

    std::string input;
    if (!std::getline(std::cin, input)) {
        return default_yes;
    }

    if (input.empty()) {
        return default_yes;
    }

    for (char& c : input) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (input == "y" || input == "yes") {
        return true;
    }
    if (input == "n" || input == "no") {
        return false;
    }

    return default_yes;
}

static int handle_run_command(lemonade::LemonadeClient& client, CliConfig& config) {
    if (!resolve_model_if_missing(client, config, "run", true)) {
        return 1;
    }

    int load_result = handle_load_command(client, config);
    if (load_result != 0) {
        return load_result;
    }

    open_url(config.host, config.port);
    return 0;
}

static int handle_recipes_command(lemonade::LemonadeClient& client, const CliConfig& config) {
    if (handle_backend_operation(config.install_backend, "Install",
        [&client](const std::string& recipe, const std::string& backend) {
            return client.install_backend(recipe, backend);
        })) {
            return 0;
    } else if (handle_backend_operation(config.uninstall_backend, "Uninstall",
        [&client](const std::string& recipe, const std::string& backend) {
            return client.uninstall_backend(recipe, backend);
        })) {
            return 0;
    }

    return client.list_recipes();
}

static int handle_launch_command(lemonade::LemonadeClient& client, CliConfig& config) {
    if (!resolve_model_if_missing(client, config, "launch", false)) {
        return 1;
    }

    bool should_import_recipe = config.use_recipe;
    if (!config.use_recipe) {
        should_import_recipe = prompt_yes_no("Do you want to import and use a recipe for launch?", false);
    }

    if (should_import_recipe) {
        std::string imported_model;
        int import_result = handle_import_remote_recipe_command(client, config, &imported_model, true);
        if (import_result != 0) {
            return import_result;
        }
        if (!imported_model.empty()) {
            config.model = imported_model;
            std::cout << "Using imported recipe model: " << config.model << std::endl;
        }
    }

    lemon_tray::AgentConfig agent_config;
    std::string config_error;

    // Build agent config
    if (!lemon_tray::build_agent_config(config.agent, config.host, config.port, config.model,
                                         agent_config, config_error)) {
        std::cerr << "Failed to build agent config: " << config_error << std::endl;
        return 1;
    }

    // Find agent binary
    const std::string agent_binary = lemon_tray::find_agent_binary(agent_config);
    if (agent_binary.empty()) {
        std::cerr << "Agent binary not found for " << config.agent << std::endl;
        if (!agent_config.install_instructions.empty()) {
            std::cerr << agent_config.install_instructions << std::endl;
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
            if (async_client.load_model(model, recipe_options) != 0) {
                std::cerr << "Async model load failed for '" << model << "'." << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Async model load error for '" << model << "': " << e.what() << std::endl;
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
        std::cerr << "Error: Failed to launch agent process: " << e.what() << std::endl;
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
    BeaconListener listener(8000, 250);
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

static int handle_scan_command(const CliConfig& config) {
    const int beacon_port = 8000;
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
    CLI::App* recipes_cmd = app.add_subcommand("recipes", "List available recipes and backends")->group("Server");
    CLI::App* status_cmd = app.add_subcommand("status", "Check server status")->group("Server");
    status_cmd->add_flag("--json", config.json_output, "Output status as JSON");
    CLI::App* logs_cmd = app.add_subcommand("logs", "Open server logs in the web UI")->group("Server");
    CLI::App* scan_cmd = app.add_subcommand("scan", "Scan for network beacons")->group("Server");

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

    // Install/uninstall options for recipes command
    recipes_cmd->add_option("--install", config.install_backend, "Install a backend (recipe:backend)")->type_name("SPEC");
    recipes_cmd->add_option("--uninstall", config.uninstall_backend, "Uninstall a backend (recipe:backend)")->type_name("SPEC");

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
    import_cmd->add_option("--repo-dir", config.repo_dir,
        "Remote recipe directory to query (e.g., claude-code)")->type_name("DIR");
    import_cmd->add_option("--recipe-file", config.recipe_file,
        "Recipe JSON filename to import from the selected remote directory")->type_name("FILE");
    import_cmd->add_flag("--skip-prompt", config.skip_prompt,
        "Run non-interactively (requires --repo-dir and --recipe-file for remote import)");
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
    launch_cmd->add_option("agent", config.agent, "Agent name to launch")
        ->required()
        ->type_name("AGENT")
        ->check(CLI::IsMember(SUPPORTED_AGENTS));
    launch_cmd->add_option("--model", config.model, "Model name to load")->type_name("MODEL");
    launch_cmd->add_flag("--use-recipe", config.use_recipe,
        "Import a recipe from the lemonade-sdk/recipes repository before launch");
    launch_cmd->add_option("--repo-dir", config.repo_dir,
        "Remote recipe directory to query when --use-recipe is enabled")->type_name("DIR");
    launch_cmd->add_option("--recipe-file", config.recipe_file,
        "Recipe JSON filename to import when --use-recipe is enabled")->type_name("FILE");
    lemon::RecipeOptions::add_cli_options(*launch_cmd, config.recipe_options);

    // Scan options
    scan_cmd->add_option("--duration", config.scan_duration, "Scan duration in seconds")->default_val(config.scan_duration)->type_name("SECONDS");

    // Parse arguments
    CLI11_PARSE(app, argc, argv);

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
    } else if (recipes_cmd->count() > 0) {
        return handle_recipes_command(client, config);
    } else if (launch_cmd->count() > 0) {
        return handle_launch_command(client, config);
    } else if (logs_cmd->count() > 0) {
        open_url(config.host, config.port, "/?logs=true");
        return 0;
    } else if (scan_cmd->count() > 0) {
        return handle_scan_command(config);
    } else {
        std::cerr << "Error: No command specified" << std::endl;
        std::cerr << app.help() << std::endl;
        return 1;
    }
}
