#include "lemon_cli/recipe_import.h"

#include "lemon/utils/http_client.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

namespace lemon_cli {
namespace {

const std::vector<std::string> kKnownKeys = {
    "checkpoint",
    "checkpoints",
    "model_name",
    "image_defaults",
    "labels",
    "recipe",
    "recipe_options",
    "size"
};

bool is_json_recipe_file(const nlohmann::json& entry) {
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

bool fetch_github_recipe_contents(const std::string& subpath,
                                  nlohmann::json& response_out,
                                  std::string& error_out) {
    std::string api_path = "/repos/lemonade-sdk/recipes/contents";
    if (!subpath.empty()) {
        api_path += "/" + subpath;
    }

    std::map<std::string, std::string> headers = {
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", "lemonade-cli"}
    };

    lemon::utils::HttpResponse res;
    try {
        res = lemon::utils::HttpClient::get("https://api.github.com" + api_path, headers);
    } catch (const std::exception& e) {
        error_out = "GitHub API request failed: " + std::string(e.what());
        return false;
    }
    if (res.status_code != 200) {
        error_out = "GitHub API request failed with status " + std::to_string(res.status_code);
        return false;
    }

    try {
        response_out = nlohmann::json::parse(res.body);
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

int prompt_numbered_choice(const std::string& title,
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

bool download_recipe_to_temp_file(const std::string& download_url,
                                  std::filesystem::path& temp_file_out,
                                  std::string& error_out) {
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    temp_file_out = std::filesystem::temp_directory_path() /
        ("lemonade-recipe-" + std::to_string(timestamp) + ".json");

    lemon::utils::DownloadResult result;
    try {
        result = lemon::utils::HttpClient::download_file(download_url, temp_file_out.string());
    } catch (const std::exception& e) {
        error_out = "Recipe download failed: " + std::string(e.what());
        return false;
    }

    if (!result.success) {
        error_out = "Recipe download failed: " + result.error_message;
        if (result.http_code > 0) {
            error_out += " (HTTP " + std::to_string(result.http_code) + ")";
        }
        return false;
    }

    return true;
}

} // namespace

bool validate_and_transform_model_json(nlohmann::json& model_data) {
    if (!model_data.contains("model_name") || !model_data["model_name"].is_string()) {
        if (model_data.contains("id") && model_data["id"].is_string()) {
            model_data["model_name"] = model_data["id"];
            model_data.erase("id");
        } else {
            std::cerr << "Error: JSON file must contain a 'model_name' string field" << std::endl;
            return false;
        }
    }

    std::string model_name = model_data["model_name"].get<std::string>();
    if (model_name.substr(0, 5) != "user.") {
        model_data["model_name"] = "user." + model_name;
    }

    if (!model_data.contains("recipe") || !model_data["recipe"].is_string()) {
        std::cerr << "Error: JSON file must contain a 'recipe' string field" << std::endl;
        return false;
    }

    bool has_checkpoints = model_data.contains("checkpoints") && model_data["checkpoints"].is_object();
    bool has_checkpoint = model_data.contains("checkpoint") && model_data["checkpoint"].is_string();
    if (!has_checkpoints && !has_checkpoint) {
        std::cerr << "Error: JSON file must contain either 'checkpoints' (object) or 'checkpoint' (string)" << std::endl;
        return false;
    }

    if (has_checkpoints && has_checkpoint) {
        model_data.erase("checkpoint");
    }

    std::vector<std::string> keys_to_remove;
    for (auto& [key, _] : model_data.items()) {
        bool is_known = false;
        for (const auto& known_key : kKnownKeys) {
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

int import_model_from_json_file(lemonade::LemonadeClient& client,
                                const std::string& json_path,
                                std::string* imported_model_out) {
    nlohmann::json model_data;

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

bool list_remote_recipe_files(const std::string& repo_dir,
                              std::vector<std::string>& recipe_files_out,
                              std::string& error_out) {
    recipe_files_out.clear();

    if (repo_dir.empty()) {
        error_out = "Recipe directory cannot be empty.";
        return false;
    }

    nlohmann::json dir_entries;
    if (!fetch_github_recipe_contents(repo_dir, dir_entries, error_out)) {
        return false;
    }

    for (const auto& entry : dir_entries) {
        if (!is_json_recipe_file(entry)) {
            continue;
        }
        recipe_files_out.push_back(entry["name"].get<std::string>());
    }

    std::sort(recipe_files_out.begin(), recipe_files_out.end());
    return true;
}

bool list_remote_recipe_directories(std::vector<std::string>& recipe_dirs_out,
                                    std::string& error_out) {
    recipe_dirs_out.clear();

    nlohmann::json top_entries;
    if (!fetch_github_recipe_contents("", top_entries, error_out)) {
        return false;
    }

    for (const auto& entry : top_entries) {
        if (!entry.is_object()) {
            continue;
        }
        if (!entry.contains("type") || !entry["type"].is_string() ||
            entry["type"].get<std::string>() != "dir") {
            continue;
        }
        if (!entry.contains("name") || !entry["name"].is_string()) {
            continue;
        }
        recipe_dirs_out.push_back(entry["name"].get<std::string>());
    }

    std::sort(recipe_dirs_out.begin(), recipe_dirs_out.end());
    return true;
}

int import_remote_recipe(lemonade::LemonadeClient& client,
                         const std::string& repo_dir,
                         const std::string& recipe_file,
                         bool skip_prompt,
                         bool yes,
                         std::string* imported_model_out,
                         bool allow_skip) {
    std::string selected_dir = repo_dir;
    const bool non_interactive = skip_prompt || yes;

    if (non_interactive && selected_dir.empty()) {
        std::cerr << "Error: Non-interactive mode requires --directory."
                  << std::endl;
        return 1;
    }
    if (non_interactive && recipe_file.empty()) {
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
    if (!recipe_file.empty()) {
        bool found = false;
        for (const auto& entry : recipe_entries) {
            if (entry["name"].get<std::string>() == recipe_file) {
                selected_entry = entry;
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Error: Recipe file '" << recipe_file
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

} // namespace lemon_cli
