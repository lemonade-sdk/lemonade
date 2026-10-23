#include "lemon_cli/pi_profile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace lemon_cli {
namespace {

constexpr const char* kPiPlaceholderApiKey = "lemonade";
constexpr const char* kPiMcpServerName = "lemonade";

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

std::string resolve_pi_mcp_path() {
    const std::string dir = resolve_pi_agent_dir();
    if (dir.empty()) {
        return "";
    }
    return (fs::path(dir) / "mcp.json").string();
}

std::string resolve_pi_settings_path() {
    const std::string dir = resolve_pi_agent_dir();
    if (dir.empty()) {
        return "";
    }
    return (fs::path(dir) / "settings.json").string();
}

nlohmann::json build_pi_model_entry(const AgentModelEntry& model) {
    nlohmann::json entry = {{"id", model.id}};

    // Pi drops every custom provider when any value in models.json is invalid,
    // so only positive windows are written. maxTokens matches the window
    // because pi clamps it to the room left after the prompt on each request.
    if (model.context_window > 0) {
        entry["contextWindow"] = model.context_window;
        entry["maxTokens"] = model.context_window;
    }

    entry["input"] = has_label(model, "vision")
        ? nlohmann::json::array({"text", "image"})
        : nlohmann::json::array({"text"});

    return entry;
}

nlohmann::json build_pi_provider_block(
    const std::string& base_url,
    const std::string& api_key,
    const std::vector<AgentModelEntry>& models) {
    nlohmann::json provider_block = nlohmann::json::object();
    provider_block["baseUrl"] = base_url;
    provider_block["api"] = "openai-completions";
    provider_block["apiKey"] = pi_api_key_value(api_key);
    // Pi's defaults target OpenAI: store and max_completion_tokens. These
    // match what pi's own llama.cpp provider sends instead.
    provider_block["compat"] = {
        {"supportsStore", false},
        {"maxTokensField", "max_tokens"},
    };

    nlohmann::json models_json = nlohmann::json::array();
    for (const auto& model : models) {
        models_json.push_back(build_pi_model_entry(model));
    }
    provider_block["models"] = std::move(models_json);

    return provider_block;
}

} // namespace

std::string pi_api_key_value(const std::string& api_key) {
    // Pi hides a provider whose key resolves to nothing, so without a Lemonade
    // key a placeholder keeps the models usable when pi runs outside launch.
    // A real key is referenced from the environment that launch exports, so
    // it never lands on disk.
    return api_key.empty() ? kPiPlaceholderApiKey : "${LEMONADE_API_KEY}";
}

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

    nlohmann::json settings;
    if (!read_json_object(settings_path, settings, error_out)) {
        return false;
    }

    settings["defaultProvider"] = provider_name;
    settings["defaultModel"] = default_model;

    return write_json_atomic(settings_path, settings, error_out);
}

bool pi_has_mcp_server() {
    const std::string mcp_path = resolve_pi_mcp_path();
    if (mcp_path.empty()) {
        return false;
    }

    nlohmann::json mcp;
    std::string ignored;
    if (!read_json_object(mcp_path, mcp, ignored)) {
        return false;
    }
    const auto servers = mcp.find("mcpServers");
    return servers != mcp.end() && servers->is_object() && servers->contains(kPiMcpServerName);
}

bool parse_pi_version(const std::string& output, int& major, int& minor) {
    static const std::regex version_pattern(R"((\d{1,6})\.(\d{1,6}))");
    std::smatch match;
    if (!std::regex_search(output, match, version_pattern)) {
        return false;
    }
    major = std::stoi(match[1].str());
    minor = std::stoi(match[2].str());
    return true;
}

bool pi_mcp_config_readable(std::string& error_out) {
    const std::string mcp_path = resolve_pi_mcp_path();
    if (mcp_path.empty()) {
        return true;
    }
    nlohmann::json mcp;
    return read_json_object(mcp_path, mcp, error_out);
}

bool sync_pi_mcp_server(const std::string& server_origin,
                        bool has_api_key,
                        std::string& error_out) {
    const std::string mcp_path = resolve_pi_mcp_path();
    if (mcp_path.empty()) {
        error_out = "Could not resolve pi mcp.json path";
        return false;
    }

    nlohmann::json mcp;
    if (!read_json_object(mcp_path, mcp, error_out)) {
        return false;
    }
    if (!mcp.contains("mcpServers") || !mcp["mcpServers"].is_object()) {
        mcp["mcpServers"] = nlohmann::json::object();
    }

    // Keys the user set on the entry (enabled, timeout, toolExposure, ...)
    // survive; only the connection details follow the current launch.
    nlohmann::json& server = mcp["mcpServers"][kPiMcpServerName];
    if (!server.is_object()) {
        server = nlohmann::json::object();
    }
    // Pi rejects a url entry whose type is not an HTTP one, and leftover
    // stdio keys would make the entry look like a local command.
    if (server.contains("type") && server["type"] != "http" &&
        server["type"] != "streamable-http") {
        server.erase("type");
    }
    for (const char* stdio_key : {"command", "args", "env", "cwd"}) {
        server.erase(stdio_key);
    }

    // Pi fails the whole connection when a header references an unset
    // variable, so the header exists only when launch exports the key.
    sync_mcp_server_connection(server, server_origin + "/mcp", "Bearer ${LEMONADE_API_KEY}",
                               has_api_key);

    // Codemode exposure would switch codemode on in every pi session; direct
    // tools keep it behind --codemode.
    if (!server.contains("exposure")) {
        server["exposure"] = "direct";
    }
    if (!server.contains("description")) {
        server["description"] =
            "Lemonade local AI server: list models, chat with local LLMs, generate images, "
            "transcribe audio, and search the Lemonade docs";
    }

    return write_json_atomic(mcp_path, mcp, error_out);
}

bool remove_pi_mcp_server(std::string& error_out) {
    const std::string mcp_path = resolve_pi_mcp_path();
    if (mcp_path.empty() || !fs::exists(mcp_path)) {
        return true;
    }

    nlohmann::json mcp;
    if (!read_json_object(mcp_path, mcp, error_out)) {
        return false;
    }
    auto servers = mcp.find("mcpServers");
    if (servers == mcp.end() || !servers->is_object() || !servers->contains(kPiMcpServerName)) {
        return true;
    }
    servers->erase(kPiMcpServerName);

    return write_json_atomic(mcp_path, mcp, error_out);
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
