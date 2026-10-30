#include "lemon_cli/opencode_profile.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace lemon_cli {
namespace {

constexpr const char* kPlaceholderApiKey = "lemonade";
constexpr const char* kMcpServerName = "lemonade";
constexpr const char* kMcpKeyHeader = "Bearer {env:LEMONADE_API_KEY}";

std::string resolve_opencode_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (xdg && xdg[0] != '\0') {
        base = xdg;
    } else {
#ifdef _WIN32
        const char* appdata = std::getenv("APPDATA");
        if (appdata && appdata[0] != '\0') {
            base = appdata;
        }
#else
        const char* home = std::getenv("HOME");
        if (home && home[0] != '\0') {
            base = (fs::path(home) / ".config").string();
        }
#endif
    }

    if (base.empty()) {
        return "";
    }

    return (fs::path(base) / "opencode" / "opencode.json").string();
}

nlohmann::json build_opencode_provider_block(
    const std::string& base_url,
    const std::string& api_key,
    const std::vector<AgentModelEntry>& models) {
    nlohmann::json provider_block = nlohmann::json::object();
    provider_block["npm"] = "@ai-sdk/openai-compatible";
    provider_block["name"] = "Lemonade Server (local)";

    nlohmann::json options = nlohmann::json::object();
    options["baseURL"] = base_url;
    // A real key is referenced from the environment that launch exports, so
    // it never lands on disk.
    options["apiKey"] = api_key.empty() ? kPlaceholderApiKey : "{env:LEMONADE_API_KEY}";
    provider_block["options"] = std::move(options);

    nlohmann::json models_json = nlohmann::json::object();
    for (const auto& model : models) {
        nlohmann::json model_entry = nlohmann::json::object();
        model_entry["name"] = model.display_name;
        if (model.context_window > 0) {
            model_entry["limit"] = nlohmann::json::object({
                {"context", model.context_window},
                {"output", model.context_window / 3}
            });
        }
        // opencode assumes a model without modalities takes images.
        if (!model.labels.empty()) {
            model_entry["modalities"] = {
                {"input", has_label(model, "vision") ? nlohmann::json::array({"text", "image"})
                                                     : nlohmann::json::array({"text"})},
                {"output", nlohmann::json::array({"text"})},
            };
        }
        models_json[model.id] = std::move(model_entry);
    }
    provider_block["models"] = std::move(models_json);

    return provider_block;
}

bool has_mcp_entry(const nlohmann::json& config) {
    const auto mcp = config.find("mcp");
    if (mcp == config.end() || !mcp->is_object()) {
        return false;
    }
    const auto servers = mcp->find("servers");
    return mcp->contains(kMcpServerName) ||
           (servers != mcp->end() && servers->is_object() && servers->contains(kMcpServerName));
}

void erase_servers_entry(nlohmann::json& mcp) {
    const auto servers = mcp.find("servers");
    if (servers == mcp.end() || !servers->is_object()) {
        return;
    }
    servers->erase(kMcpServerName);
    if (servers->empty()) {
        mcp.erase(servers);
    }
}

} // namespace

const AgentConfigProfile& opencode_profile() {
    static const AgentConfigProfile profile = {
        "OpenCode",
        resolve_opencode_config_path,
        build_opencode_provider_block,
        "provider",
        nlohmann::json{{"$schema", "https://opencode.ai/config.json"}},
    };

    return profile;
}

OpencodeMcpShape opencode_mcp_shape(std::optional<int> major_version) {
    if (major_version) {
        return *major_version >= 2 ? OpencodeMcpShape::Servers : OpencodeMcpShape::Legacy;
    }
    // Moving a v2 entry to mcp.<name> would turn its Code Mode back on, so
    // without a version an existing entry keeps its shape.
    nlohmann::json config;
    std::string ignored;
    if (read_json_object(resolve_opencode_config_path(), config, ignored) &&
        config.contains(nlohmann::json::json_pointer("/mcp/servers/lemonade"))) {
        return OpencodeMcpShape::Servers;
    }
    return OpencodeMcpShape::Legacy;
}

bool opencode_has_mcp_server() {
    nlohmann::json config;
    std::string ignored;
    return read_json_object(resolve_opencode_config_path(), config, ignored) &&
           has_mcp_entry(config);
}

bool sync_opencode_mcp_server(const std::string& server_origin,
                              bool has_api_key,
                              OpencodeMcpShape shape,
                              std::string& error_out) {
    const std::string config_path = resolve_opencode_config_path();
    if (config_path.empty()) {
        error_out = "Could not resolve opencode.json path";
        return false;
    }

    nlohmann::json config;
    if (!read_json_object(config_path, config, error_out)) {
        return false;
    }
    if (!config.contains("mcp") || !config["mcp"].is_object()) {
        config["mcp"] = nlohmann::json::object();
    }
    nlohmann::json& mcp = config["mcp"];

    // Keys differ between the shapes (enabled vs disabled, timeout), so a
    // moved entry starts over instead of carrying them.
    nlohmann::json* server = nullptr;
    if (shape == OpencodeMcpShape::Servers) {
        mcp.erase(kMcpServerName);
        if (!mcp.contains("servers") || !mcp["servers"].is_object()) {
            mcp["servers"] = nlohmann::json::object();
        }
        server = &mcp["servers"][kMcpServerName];
    } else {
        erase_servers_entry(mcp);
        server = &mcp[kMcpServerName];
    }
    if (!server->is_object()) {
        *server = nlohmann::json::object();
    }

    // Leftover local keys would make the entry look like a command.
    for (const char* local_key : {"command", "environment", "cwd"}) {
        server->erase(local_key);
    }
    (*server)["type"] = "remote";
    sync_mcp_server_connection(*server, server_origin + "/mcp", kMcpKeyHeader, has_api_key);

    // Without oauth:false opencode probes the server for OAuth. Code Mode
    // stays off unless launch turns it on with --codemode.
    if (!server->contains("oauth")) {
        (*server)["oauth"] = false;
    }
    if (shape == OpencodeMcpShape::Servers && !server->contains("codemode")) {
        (*server)["codemode"] = false;
    }

    return write_json_atomic(config_path, config, error_out);
}

bool remove_opencode_mcp_server(std::string& error_out) {
    const std::string config_path = resolve_opencode_config_path();
    if (config_path.empty() || !fs::exists(config_path)) {
        return true;
    }

    nlohmann::json config;
    if (!read_json_object(config_path, config, error_out)) {
        return false;
    }
    if (!has_mcp_entry(config)) {
        return true;
    }
    nlohmann::json& mcp = config["mcp"];
    mcp.erase(kMcpServerName);
    erase_servers_entry(mcp);

    return write_json_atomic(config_path, config, error_out);
}

nlohmann::json opencode_codemode_config() {
    nlohmann::json config;
    std::string ignored;
    if (!read_json_object(resolve_opencode_config_path(), config, ignored)) {
        return nullptr;
    }
    const nlohmann::json::json_pointer pointer("/mcp/servers/lemonade");
    if (!config.contains(pointer) || !config[pointer].is_object()) {
        return nullptr;
    }
    // opencode ignores a partial entry here, so the whole entry is passed.
    nlohmann::json server = config[pointer];
    server["codemode"] = true;
    return {{"mcp", {{"servers", {{kMcpServerName, std::move(server)}}}}}};
}

} // namespace lemon_cli
