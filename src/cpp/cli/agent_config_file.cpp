#include "lemon_cli/agent_config_file.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace lemon_cli {

bool has_label(const AgentModelEntry& model, const std::string& label) {
    return std::find(model.labels.begin(), model.labels.end(), label) != model.labels.end();
}

bool write_json_atomic(const std::string& path,
                       const nlohmann::json& config,
                       std::string& error_out) {
    fs::path output_path(path);
    std::error_code ec;
    if (fs::is_symlink(output_path, ec)) {
        const fs::path target = fs::canonical(output_path, ec);
        if (ec) {
            error_out = "Cannot resolve symlink " + output_path.string() + ": " + ec.message();
            return false;
        }
        output_path = target;
    }
    ec.clear();
    const fs::file_status existing = fs::status(output_path, ec);
    const bool keep_permissions = !ec && fs::exists(existing);
    ec.clear();
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

    if (keep_permissions) {
        fs::permissions(tmp_path, existing.permissions(), ec);
        if (ec) {
            std::error_code remove_ec;
            fs::remove(tmp_path, remove_ec);
            error_out = "Cannot set permissions on " + tmp_path.string() + ": " + ec.message();
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

bool read_json_object(const std::string& path, nlohmann::json& out, std::string& error_out) {
    out = nlohmann::json::object();
    if (!fs::exists(path)) {
        return true;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        error_out = "Cannot open " + path + " for reading";
        return false;
    }

    try {
        out = nlohmann::json::parse(file);
    } catch (const nlohmann::json::exception& e) {
        error_out = "Cannot parse existing " + path + ": " + std::string(e.what()) +
                    ". Refusing to overwrite. Fix or remove the file manually.";
        return false;
    }

    if (!out.is_object()) {
        error_out = path + " is not a JSON object. Refusing to overwrite.";
        return false;
    }
    return true;
}

void sync_mcp_server_connection(nlohmann::json& server,
                                const std::string& url,
                                const std::string& key_header,
                                bool has_api_key) {
    const auto old_url = server.find("url");
    if (old_url == server.end() || *old_url != url) {
        for (const char* credential_key : {"headers", "oauth", "auth"}) {
            server.erase(credential_key);
        }
    }
    server["url"] = url;

    if (has_api_key) {
        if (!server.contains("headers") || !server["headers"].is_object()) {
            server["headers"] = nlohmann::json::object();
        }
        server["headers"]["Authorization"] = key_header;
    } else if (server.contains("headers") && server["headers"].is_object()) {
        const auto auth = server["headers"].find("Authorization");
        if (auth != server["headers"].end() && *auth == key_header) {
            server["headers"].erase(auth);
        }
        if (server["headers"].empty()) {
            server.erase("headers");
        }
    }
}

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
    if (!read_json_object(config_path, config, error_out)) {
        return false;
    }

    if (profile.initial_skeleton.is_object()) {
        for (auto it = profile.initial_skeleton.begin(); it != profile.initial_skeleton.end(); ++it) {
            if (!config.contains(it.key())) {
                config[it.key()] = it.value();
            }
        }
    }

    if (!config.contains(profile.provider_key) || !config[profile.provider_key].is_object()) {
        config[profile.provider_key] = nlohmann::json::object();
    }
    config[profile.provider_key][provider_name] =
        profile.build_provider_block(base_url, api_key, models);

    return write_json_atomic(config_path, config, error_out);
}

} // namespace lemon_cli
