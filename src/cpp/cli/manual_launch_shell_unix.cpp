#include "manual_launch_shell.h"

namespace lemon_cli {
namespace {

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
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
    const std::string key_ref = "\"${" + key_env + "}\"";
    if (value == api_key) return key_ref;
    std::string result;
    size_t begin = 0;
    size_t pos = 0;
    while ((pos = value.find(api_key, begin)) != std::string::npos) {
        result += shell_quote(value.substr(begin, pos - begin));
        result += key_ref;
        begin = pos + api_key.size();
    }
    if (begin < value.size()) {
        result += shell_quote(value.substr(begin));
    }
    return result;
}

} // namespace

void print_manual_shell_recipe(const lemon_tray::AgentConfig& agent_config,
                               const std::string& api_key,
                               const std::string& key_env,
                               std::ostream& out) {
    out << "\nManual launch (POSIX shell):\n";
    if (!api_key.empty()) {
        out << "Set " << key_env << " securely in this shell before running "
               "the commands below; do not paste the key into them.\n";
    }
    out << "Environment variables:\n";
    if (!api_key.empty()) {
        out << "  : \"${" << key_env << ":?Set " << key_env << " first}\"\n";
        out << "  export " << key_env << '\n';
    }
    for (const auto& [key, value] : agent_config.env_vars) {
        if (!api_key.empty() && key == key_env && value == api_key) {
            continue;  // The caller supplies the effective credential.
        }
        out << "  export " << key << '=' << shell_quote_arg(value, api_key, key_env) << '\n';
    }
    out << "Command:\n  " << shell_quote(agent_config.binary_name);
    for (const auto& arg : agent_config.extra_args) {
        out << ' ' << shell_quote_arg(arg, api_key, key_env);
    }
    out << '\n';
}

} // namespace lemon_cli
