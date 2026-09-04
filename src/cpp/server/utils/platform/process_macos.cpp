#include "process_unix_base.h"
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/path_utils.h>
#include <lemon/sandbox/env_scrubber.h>
#include <nlohmann/json.hpp>

#include <crt_externs.h>
#include <errno.h>
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace lemon::utils {

class MacOSProcessPlatform : public UnixProcessPlatform {
protected:
    pid_t spawn_process(
        const std::string& executable,
        const std::vector<std::string>& args,
        const std::string& working_dir,
        bool inherit_output,
        bool filter_health_logs,
        const std::vector<std::pair<std::string, std::string>>& env_vars,
        int stdout_pipe[2],
        int stderr_pipe[2],
        const std::optional<lemon::sandbox::SandboxPolicy>& sandbox_policy = std::nullopt) override;
};

pid_t MacOSProcessPlatform::spawn_process(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    bool inherit_output,
    bool filter_health_logs,
    const std::vector<std::pair<std::string, std::string>>& env_vars,
    int stdout_pipe[2],
    int stderr_pipe[2],
    const std::optional<lemon::sandbox::SandboxPolicy>& sandbox_policy) {

    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);

    if (stdout_pipe[1] >= 0) {
        posix_spawn_file_actions_addclose(&file_actions, stdout_pipe[0]);
        posix_spawn_file_actions_adddup2(&file_actions, stdout_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&file_actions, stdout_pipe[1]);
    } else if (!inherit_output) {
        posix_spawn_file_actions_addopen(&file_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }

    if (stderr_pipe[1] >= 0) {
        posix_spawn_file_actions_addclose(&file_actions, stderr_pipe[0]);
        posix_spawn_file_actions_adddup2(&file_actions, stderr_pipe[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&file_actions, stderr_pipe[1]);
    } else if (!inherit_output) {
        posix_spawn_file_actions_addopen(&file_actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }

    if (!working_dir.empty()) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        posix_spawn_file_actions_addchdir_np(&file_actions, working_dir.c_str());
#pragma clang diagnostic pop
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t default_signals;
    sigfillset(&default_signals);
    posix_spawnattr_setsigdefault(&attr, &default_signals);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGDEF);

    std::vector<std::string> env_strings;
    const bool should_sanitize = sandbox_policy.has_value() &&
                                 sandbox_policy->mode != lemon::sandbox::SandboxMode::Disabled;
    if (should_sanitize) {
        std::vector<std::pair<std::string, std::string>> combined_env = env_vars;
        for (const auto& kv : sandbox_policy->explicit_env_vars) {
            combined_env.push_back(kv);
        }
        auto sanitized_env = lemon::sandbox::EnvScrubber::sanitize_environment(
            combined_env, sandbox_policy->allowed_env_vars, true);

        env_strings.reserve(sanitized_env.size());
        for (const auto& env_pair : sanitized_env) {
            env_strings.emplace_back(env_pair.first + "=" + env_pair.second);
        }
    } else {
        char** env_curr = (*_NSGetEnviron());
        if (env_curr != nullptr) {
            for (char** env = env_curr; *env != nullptr; ++env) {
                env_strings.emplace_back(*env);
            }
        }
        for (const auto& env_pair : env_vars) {
            std::string prefix = env_pair.first + "=";
            bool found = false;
            for (auto& entry : env_strings) {
                if (entry.rfind(prefix, 0) == 0) {
                    entry = prefix + env_pair.second;
                    found = true;
                    break;
                }
            }
            if (!found) {
                env_strings.emplace_back(prefix + env_pair.second);
            }
        }
    }

    std::vector<char*> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto& s : env_strings) {
        envp.push_back(&s[0]);
    }
    envp.push_back(nullptr);

    std::string exec_bin = executable;
    std::vector<std::string> actual_args = args;
    std::string policy_json_str;

    const bool should_enforce_sandbox = sandbox_policy.has_value() &&
                                        (sandbox_policy->mode == lemon::sandbox::SandboxMode::Auto ||
                                         sandbox_policy->mode == lemon::sandbox::SandboxMode::Enforced);

    if (should_enforce_sandbox) {
        std::string trampoline_path;
        std::string candidate1 = lemon::utils::get_downloaded_bin_dir() + "/lemonade-sandbox-exec";
        if (std::filesystem::exists(candidate1)) {
            trampoline_path = candidate1;
        } else if (std::filesystem::exists("/usr/local/bin/lemonade-sandbox-exec")) {
            trampoline_path = "/usr/local/bin/lemonade-sandbox-exec";
        } else if (std::filesystem::exists("/opt/homebrew/bin/lemonade-sandbox-exec")) {
            trampoline_path = "/opt/homebrew/bin/lemonade-sandbox-exec";
        }

        if (!trampoline_path.empty()) {
            nlohmann::json j = *sandbox_policy;
            policy_json_str = j.dump();

            exec_bin = trampoline_path;
            actual_args.clear();
            actual_args.push_back("--policy");
            actual_args.push_back(policy_json_str);
            actual_args.push_back("--");
            actual_args.push_back(executable);
            for (const auto& a : args) {
                actual_args.push_back(a);
            }
        } else {
            if (sandbox_policy->mode == lemon::sandbox::SandboxMode::Enforced) {
                posix_spawn_file_actions_destroy(&file_actions);
                posix_spawnattr_destroy(&attr);
                throw std::runtime_error("lemonade-sandbox-exec helper missing for enforced sandbox");
            }
        }
    }

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(actual_args.size() + 2);
    argv_ptrs.push_back(const_cast<char*>(exec_bin.c_str()));
    for (const auto& arg : actual_args) {
        argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
    }
    argv_ptrs.push_back(nullptr);

    pid_t pid = 0;
    int spawn_result = posix_spawnp(&pid, exec_bin.c_str(), &file_actions, &attr, argv_ptrs.data(), envp.data());

    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);

    if (spawn_result != 0) {
        return 0;
    }

    return pid;
}

std::unique_ptr<ProcessPlatform> create_process_platform() {
    return std::make_unique<MacOSProcessPlatform>();
}

} // namespace lemon::utils
