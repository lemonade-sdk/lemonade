#include "lemon_cli/agent_config_file.h"

#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace lemon_cli {

bool sync_agent_config_file(const AgentConfigProfile& profile,
                            const std::string& provider_name,
                            const std::string& base_url,
                            const std::string& api_key,
                            const std::vector<AgentModelEntry>& models,
                            std::string& error_out) {
    const std::string config_path = profile.resolve_config_path();
    if (config_path.empty()) {
        error_out = "Could not resolve config path for " + profile.agent_name;
        return false;
    }

    nlohmann::json config;

    if (fs::exists(config_path)) {
        std::ifstream file(config_path);
        if (!file.is_open()) {
            error_out = "Cannot open " + config_path + " for reading";
            return false;
        }

        try {
            config = nlohmann::json::parse(file);
        } catch (const nlohmann::json::exception& e) {
            error_out = "Cannot parse existing " + config_path + ": " +
                        std::string(e.what()) +
                        ". Refusing to overwrite. Fix or remove the file manually.";
            return false;
        }

        if (!config.is_object()) {
            error_out = config_path + " is not a JSON object. Refusing to overwrite.";
            return false;
        }

        if (profile.initial_skeleton.is_object()) {
            for (auto it = profile.initial_skeleton.begin(); it != profile.initial_skeleton.end(); ++it) {
                if (!config.contains(it.key())) {
                    config[it.key()] = it.value();
                }
            }
        }
    } else {
        config = profile.initial_skeleton;
        if (!config.is_object()) {
            config = nlohmann::json::object();
        }
    }

    if (!config.contains(profile.provider_key) || !config[profile.provider_key].is_object()) {
        config[profile.provider_key] = nlohmann::json::object();
    }

    nlohmann::json provider_block =
        profile.build_provider_block(base_url, api_key, models);

    auto& providers = config[profile.provider_key];
    providers[provider_name] = std::move(provider_block);

    const fs::path output_path(config_path);
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
        // Fall back to copy-overwrite without deleting the existing file first.
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

} // namespace lemon_cli
