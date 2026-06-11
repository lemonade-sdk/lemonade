#include "lemon_cli/pi_profile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace lemon_cli {
namespace {

// Pi stores its config under ~/.pi/agent/ (%USERPROFILE%\.pi\agent\ on Windows)
// by default, or in $PI_CODING_AGENT_DIR if set. It does not honor XDG_CONFIG_HOME.
std::string resolve_pi_agent_dir() {
    const char* pi_dir = std::getenv("PI_CODING_AGENT_DIR");
    if (pi_dir && pi_dir[0] != '\0') {
        return std::string(pi_dir);
    }

#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home || home[0] == '\0') {
        return "";
    }
    return (fs::path(home) / ".pi" / "agent").string();
}

std::string resolve_pi_models_path() {
    const std::string dir = resolve_pi_agent_dir();
    if (dir.empty()) {
        return "";
    }
    return (fs::path(dir) / "models.json").string();
}

std::string resolve_pi_settings_path() {
    const std::string dir = resolve_pi_agent_dir();
    if (dir.empty()) {
        return "";
    }
    return (fs::path(dir) / "settings.json").string();
}

nlohmann::json build_pi_provider_block(
    const std::string& base_url,
    const std::string& api_key,
    const std::vector<AgentModelEntry>& models) {
    nlohmann::json provider_block = nlohmann::json::object();
    provider_block["baseUrl"] = base_url;
    provider_block["api"] = "openai-completions";
    if (!api_key.empty()) {
        provider_block["apiKey"] = api_key;
    }

    nlohmann::json models_json = nlohmann::json::array();
    for (const auto& model : models) {
        models_json.push_back(nlohmann::json{{"id", model.id}});
    }
    provider_block["models"] = std::move(models_json);

    return provider_block;
}

// Atomically write JSON to path, creating parent directories. Mirrors the
// approach in agent_config_file.cpp.
bool write_json_atomic(const std::string& path,
                       const nlohmann::json& config,
                       std::string& error_out) {
    const fs::path output_path(path);
    std::error_code ec;
    const fs::path output_dir = output_path.parent_path();
    if (!output_dir.empty()) {
        fs::create_directories(output_dir, ec);
        if (ec) {
            error_out = "Cannot create directory " + output_dir.string() + ": " + ec.message();
            return false;
        }
    }

    const fs::path tmp_path = output_path.string() + ".tmp";
    {
        std::ofstream out(tmp_path);
        if (!out.is_open()) {
            error_out = "Cannot open " + tmp_path.string() + " for writing";
            return false;
        }

        out << config.dump(2) << "\n";
        out.flush();
        if (!out.good()) {
            out.close();
            std::error_code remove_ec;
            fs::remove(tmp_path, remove_ec);
            error_out = "Write failed for " + tmp_path.string();
            return false;
        }

        out.close();
        if (!out) {
            std::error_code remove_ec;
            fs::remove(tmp_path, remove_ec);
            error_out = "Write failed for " + tmp_path.string();
            return false;
        }
    }

    fs::rename(tmp_path, output_path, ec);
    if (ec) {
        // Cross-device or filesystem edge cases can make rename fail.
        ec.clear();
        fs::copy_file(tmp_path, output_path, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::error_code remove_tmp_ec;
            fs::remove(tmp_path, remove_tmp_ec);
        }
    }

    if (ec) {
        std::error_code remove_ec;
        fs::remove(tmp_path, remove_ec);
        error_out = "Cannot move temp config into place: " + ec.message();
        return false;
    }

    return true;
}

} // namespace

const AgentConfigProfile& pi_profile() {
    static const AgentConfigProfile profile = {
        "Pi",
        resolve_pi_models_path,
        build_pi_provider_block,
        "providers",
        nlohmann::json::object(),
    };

    return profile;
}

bool sync_pi_settings_file(const std::string& provider_name,
                           const std::string& default_model,
                           std::string& error_out) {
    const std::string settings_path = resolve_pi_settings_path();
    if (settings_path.empty()) {
        error_out = "Could not resolve pi settings path";
        return false;
    }

    nlohmann::json settings = nlohmann::json::object();

    if (fs::exists(settings_path)) {
        std::ifstream file(settings_path);
        if (!file.is_open()) {
            error_out = "Cannot open " + settings_path + " for reading";
            return false;
        }

        try {
            settings = nlohmann::json::parse(file);
        } catch (const nlohmann::json::exception& e) {
            error_out = "Cannot parse existing " + settings_path + ": " +
                        std::string(e.what()) +
                        ". Refusing to overwrite. Fix or remove the file manually.";
            return false;
        }

        if (!settings.is_object()) {
            error_out = settings_path + " is not a JSON object. Refusing to overwrite.";
            return false;
        }
    }

    settings["defaultProvider"] = provider_name;
    settings["defaultModel"] = default_model;

    return write_json_atomic(settings_path, settings, error_out);
}

bool pi_has_default_config() {
    const std::string settings_path = resolve_pi_settings_path();
    if (settings_path.empty() || !fs::exists(settings_path)) {
        return false;
    }

    std::ifstream file(settings_path);
    if (!file.is_open()) {
        return false;
    }

    try {
        nlohmann::json settings = nlohmann::json::parse(file);
        if (!settings.is_object()) {
            return false;
        }
        return settings.contains("defaultProvider") && settings["defaultProvider"].is_string() &&
               settings.contains("defaultModel") && settings["defaultModel"].is_string();
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

} // namespace lemon_cli
