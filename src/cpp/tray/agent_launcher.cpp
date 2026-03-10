#include "lemon_tray/agent_launcher.h"

#include <lemon/utils/path_utils.h>

#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

namespace lemon_tray {
namespace {

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
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

} // namespace

bool build_agent_config(const std::string& agent,
                        const std::string& host,
                        int port,
                        const std::string& model,
                        AgentConfig& config,
                        std::string& error_message) {
    const std::string base = "http://" + host + ":" + std::to_string(port);

    if (agent == "claude") {
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
#ifdef _WIN32
        const char* appdata = std::getenv("APPDATA");
        if (appdata) {
            config.fallback_paths.push_back(std::string(appdata) + "\\npm\\claude.cmd");
            config.fallback_paths.push_back(std::string(appdata) + "\\npm\\claude.exe");
        }
#endif
        config.env_vars = {
            // Claude Code sends requests to /v1/messages relative to ANTHROPIC_BASE_URL.
            // Keep this as origin-only to avoid /v1/v1/messages.
            {"ANTHROPIC_BASE_URL", base},
            {"ANTHROPIC_API_KEY", "lemonade"},
            {"ANTHROPIC_AUTH_TOKEN", "lemonade"},
            {"ANTHROPIC_DEFAULT_OPUS_MODEL", model},
            {"ANTHROPIC_DEFAULT_SONNET_MODEL", model},
            {"ANTHROPIC_DEFAULT_HAIKU_MODEL", model},
            {"CLAUDE_CODE_SUBAGENT_MODEL", model},
            {"CLAUDE_CODE_ATTRIBUTION_HEADER", "0"},
            {"CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", "1"}
        };
        config.extra_args = {};
        config.install_instructions = "Install Claude Code CLI and ensure 'claude' is on PATH.";
        return true;
    }

    if (agent == "codex") {
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
#ifdef _WIN32
        const char* appdata = std::getenv("APPDATA");
        if (appdata) {
            config.fallback_paths.push_back(std::string(appdata) + "\\npm\\codex.cmd");
            config.fallback_paths.push_back(std::string(appdata) + "\\npm\\codex.exe");
        }
#endif
        config.env_vars = {
            {"OPENAI_BASE_URL", base + "/v1/"},
            {"OPENAI_API_KEY", "lemonade"}
        };
        config.extra_args = {
            "--oss",
            "-m",
            model,
            "--config",
            "web_search=\"disabled\""
        };
        config.install_instructions = "Install Codex CLI and ensure 'codex' is on PATH.";
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
