#include "lemon_cli/opencode_profile.h"

#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace lemon_cli {
namespace {

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
    if (!api_key.empty()) {
        options["apiKey"] = api_key;
    }
    provider_block["options"] = std::move(options);

    nlohmann::json models_json = nlohmann::json::object();
    for (const auto& model : models) {
        nlohmann::json model_entry = nlohmann::json::object();
        model_entry["name"] = model.display_name;
        model_entry["contextWindow"] = model.context_window;
        models_json[model.id] = std::move(model_entry);
    }
    provider_block["models"] = std::move(models_json);

    return provider_block;
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

} // namespace lemon_cli
