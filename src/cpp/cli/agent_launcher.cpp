#include "lemon_cli/agent_launcher.h"

#include <lemon/utils/path_utils.h>

#include <filesystem>
#include <cstdlib>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon_tray {
namespace {

constexpr const char* kDefaultAgentApiKey = "lemonade";

std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') {
        return path;
    }

#ifdef _WIN32
    const char* user_profile = std::getenv("USERPROFILE");
    if (!user_profile) {
        return path;
    }
    if (path.size() == 1) {
        return std::string(user_profile);
    }
    if (path[1] == '\\' || path[1] == '/') {
        return std::string(user_profile) + path.substr(1);
    }
    return path;
#else
    const char* home = std::getenv("HOME");
    if (!home) {
        return path;
    }
    if (path.size() == 1) {
        return std::string(home);
    }
    if (path[1] == '/') {
        return std::string(home) + path.substr(1);
    }
    return path;
#endif
}

bool file_is_executable(const std::string& candidate) {
    if (candidate.empty()) {
        return false;
    }

    std::error_code ec;
    fs::path p(candidate);
    if (!(fs::exists(p, ec) && fs::is_regular_file(p, ec))) {
        return false;
    }

#ifdef _WIN32
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

void add_windows_npm_fallbacks(std::vector<std::string>& fallback_paths,
                               const std::string& binary_base_name) {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        fallback_paths.push_back(std::string(appdata) + "\\npm\\" + binary_base_name + ".cmd");
        fallback_paths.push_back(std::string(appdata) + "\\npm\\" + binary_base_name + ".exe");
    }
#else
    (void)fallback_paths;
    (void)binary_base_name;
#endif
}

std::string normalize_server_host(const std::string& host) {
    if (host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]" || host == "*") {
        return "localhost";
    }
    return host;
}

std::string build_server_base_url(const std::string& host, int port) {
    return "http://" + normalize_server_host(host) + ":" + std::to_string(port);
}

void configure_claude_agent(const std::string& base_url,
                            const std::string& model,
                            const std::string& api_key,
                            AgentConfig& config) {
    const std::string resolved_api_key = api_key.empty() ? kDefaultAgentApiKey : api_key;

    config.binary_name = "claude";
#ifdef _WIN32
    config.binary_alternatives = {"claude.cmd", "claude.exe"};
#else
    config.binary_alternatives = {};
#endif
    config.fallback_paths = {
        "~/.npm-global/bin/claude",
        "/usr/local/bin/claude"
    };
    add_windows_npm_fallbacks(config.fallback_paths, "claude");

    config.env_vars = {
        // Claude Code sends requests to /v1/messages relative to ANTHROPIC_BASE_URL.
        // Keep this as origin-only to avoid /v1/v1/messages.
        {"ANTHROPIC_BASE_URL", base_url},
        {"ANTHROPIC_AUTH_TOKEN", resolved_api_key},
        // We want to keep this for agents that run workflows which query endpoints with auth
        {"LEMONADE_API_KEY", resolved_api_key},
        {"ANTHROPIC_DEFAULT_OPUS_MODEL", model},
        {"ANTHROPIC_DEFAULT_SONNET_MODEL", model},
        {"ANTHROPIC_DEFAULT_HAIKU_MODEL", model},
        {"CLAUDE_CODE_SUBAGENT_MODEL", model},
        {"CLAUDE_CODE_ATTRIBUTION_HEADER", "0"},
        {"CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", "1"}
    };
    config.extra_args = {};
    config.install_instructions = "Install Claude Code CLI and ensure 'claude' is on PATH.";
}

void configure_codex_agent(const std::string& base_url,
                           const std::string& model,
                           const std::string& api_key,
                           AgentConfig& config) {
    const std::string resolved_api_key = api_key.empty() ? kDefaultAgentApiKey : api_key;

    config.binary_name = "codex";
#ifdef _WIN32
    config.binary_alternatives = {"codex.cmd", "codex.exe"};
#else
    config.binary_alternatives = {};
#endif
    config.fallback_paths = {
        "~/.npm-global/bin/codex",
        "/usr/local/bin/codex"
    };
    add_windows_npm_fallbacks(config.fallback_paths, "codex");

    config.env_vars = {
        {"OPENAI_BASE_URL", base_url + "/v1/"},
        {"OPENAI_API_KEY", resolved_api_key},
        {"LEMONADE_API_KEY", resolved_api_key}
    };
    config.extra_args = {
        "--oss",
        "-m",
        model,
        "--config",
        "web_search=\"disabled\""
    };
    config.install_instructions = "Install Codex CLI and ensure 'codex' is on PATH.";
}

} // namespace

bool build_agent_config(const std::string& agent,
                        const std::string& host,
                        int port,
                        const std::string& model,
                        const std::string& api_key,
                        AgentConfig& config,
                        std::string& error_message) {
    const std::string base = build_server_base_url(host, port);

    if (agent == "claude") {
        configure_claude_agent(base, model, api_key, config);
        return true;
    }

    if (agent == "codex") {
        configure_codex_agent(base, model, api_key, config);
        return true;
    }

    error_message = "Unsupported agent: " + agent + ". Supported agents: claude, codex.";
    return false;
}

std::string find_agent_binary(const AgentConfig& config) {
    std::string found = lemon::utils::find_executable_in_path(config.binary_name);
    if (!found.empty()) {
        return found;
    }

    for (const auto& alt : config.binary_alternatives) {
        found = lemon::utils::find_executable_in_path(alt);
        if (!found.empty()) {
            return found;
        }
    }

    for (const auto& fallback : config.fallback_paths) {
        std::string expanded = expand_home(fallback);
        if (file_is_executable(expanded)) {
            return expanded;
        }
    }

    return "";
}

} // namespace lemon_tray
