#include "manual_launch_shell.h"

namespace lemon_cli {
namespace {

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        // PowerShell treats U+2018..U+201B as quote delimiters.
        if (value.compare(i, 3, "\xE2\x80\x98") == 0 ||
            value.compare(i, 3, "\xE2\x80\x99") == 0 ||
            value.compare(i, 3, "\xE2\x80\x9A") == 0 ||
            value.compare(i, 3, "\xE2\x80\x9B") == 0) {
            quoted += value.substr(i, 3);
            quoted += value.substr(i, 3);
            i += 2;
            continue;
        }
        if (ch == '\'') {
            quoted += "''";
        } else {
            quoted += ch;
        }
    }
    return quoted + "'";
}

std::string shell_quote_arg(const std::string& value, const std::string& api_key,
                            const std::string& key_env) {
    if (api_key.empty() || value.find(api_key) == std::string::npos) {
        return shell_quote(value);
    }
    const std::string key_ref = "$env:" + key_env;
    if (value == api_key) return key_ref;
    std::string result = "(";
    bool first = true;
    size_t begin = 0;
    size_t pos = 0;
    while ((pos = value.find(api_key, begin)) != std::string::npos) {
        if (pos > begin) {
            if (!first) result += " + ";
            result += shell_quote(value.substr(begin, pos - begin));
            first = false;
        }
        if (!first) result += " + ";
        result += key_ref;
        first = false;
        begin = pos + api_key.size();
    }
    if (begin < value.size()) {
        if (!first) result += " + ";
        result += shell_quote(value.substr(begin));
    }
    return result + ")";
}

} // namespace

void print_manual_shell_recipe(const lemon_tray::AgentConfig& agent_config,
                               const std::string& api_key,
                               const std::string& key_env,
                               std::ostream& out) {
    out << "\nManual launch (PowerShell):\n";
    if (!api_key.empty()) {
        out << "Set " << key_env << " securely in this shell before running "
               "the commands below; do not paste the key into them.\n";
    }
    out << "Environment variables:\n";
    if (!api_key.empty()) {
        out << "  if (-not $env:" << key_env << ") { throw 'Set "
            << key_env << " first' }\n";
    }
    for (const auto& [key, value] : agent_config.env_vars) {
        if (!api_key.empty() && key == key_env && value == api_key) {
            continue;  // The caller supplies the effective credential.
        }
        out << "  $env:" << key << " = " << shell_quote_arg(value, api_key, key_env) << '\n';
    }
    out << "Command:\n  & " << shell_quote(agent_config.binary_name);
    for (const auto& arg : agent_config.extra_args) {
        out << ' ' << shell_quote_arg(arg, api_key, key_env);
    }
    out << '\n';
    if (!agent_config.binary_alternatives.empty()) {
        out << "Use the installed entry point (" << agent_config.binary_name;
        for (const auto& alt : agent_config.binary_alternatives) {
            out << ", " << alt;
        }
        out << ") in the command.\n";
    }
}

} // namespace lemon_cli
