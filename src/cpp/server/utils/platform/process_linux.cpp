#include "process_unix_base.h"
#include <lemon/utils/aixlog.hpp>
#include <lemon/sandbox/env_scrubber.h>
#include <lemon/sandbox/nono_ffi.h>
#include <lemon/sandbox/sandbox_engine.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HAVE_LIBCAP
#include <sys/capability.h>
#endif

namespace lemon::utils {

#ifdef HAVE_LIBCAP
static void preserve_capabilities_for_exec() {
    cap_t caps = cap_get_proc();
    if (!caps) {
        return;
    }

    cap_flag_value_t has_sys_resource = CAP_CLEAR;
    cap_get_flag(caps, CAP_SYS_RESOURCE, CAP_EFFECTIVE, &has_sys_resource);

    if (has_sys_resource == CAP_SET) {
        cap_value_t cap_list[] = {CAP_SYS_RESOURCE};

        if (cap_set_flag(caps, CAP_INHERITABLE, 1, cap_list, CAP_SET) == 0) {
            if (cap_set_proc(caps) == 0) {
                prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_SYS_RESOURCE, 0, 0);
            }
        }
    }

    cap_free(caps);
}
#endif

class LinuxProcessPlatform : public UnixProcessPlatform {
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

    bool is_zombie(pid_t pid) const override;
};

bool LinuxProcessPlatform::is_zombie(pid_t pid) const {
    std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
    std::string stat_line;
    if (!std::getline(stat_file, stat_line)) {
        return false;
    }

    const auto close_paren = stat_line.rfind(')');
    return close_paren != std::string::npos &&
           close_paren + 2 < stat_line.size() &&
           stat_line[close_paren + 2] == 'Z';
}

pid_t LinuxProcessPlatform::spawn_process(
    const std::string& executable,
    const std::vector<std::string>& args,
    const std::string& working_dir,
    bool inherit_output,
    bool filter_health_logs,
    const std::vector<std::pair<std::string, std::string>>& env_vars,
    int stdout_pipe[2],
    int stderr_pipe[2],
    const std::optional<lemon::sandbox::SandboxPolicy>& sandbox_policy) {

    std::vector<std::pair<std::string, std::string>> sanitized_env;
    const bool should_sanitize = sandbox_policy.has_value() &&
                                 sandbox_policy->mode != lemon::sandbox::SandboxMode::Disabled;
    if (should_sanitize) {
        std::vector<std::pair<std::string, std::string>> combined_env = env_vars;
        for (const auto& kv : sandbox_policy->explicit_env_vars) {
            combined_env.push_back(kv);
        }
        sanitized_env = lemon::sandbox::EnvScrubber::sanitize_environment(
            combined_env, sandbox_policy->allowed_env_vars, true);
    }

    nono_capability_set* prebuilt_caps = nullptr;
    const bool should_enforce_sandbox = sandbox_policy.has_value() &&
                                        (sandbox_policy->mode == lemon::sandbox::SandboxMode::Auto ||
                                         sandbox_policy->mode == lemon::sandbox::SandboxMode::Enforced);
    if (should_enforce_sandbox) {
        prebuilt_caps = nono_capability_set_new();
        if (prebuilt_caps) {
            lemon::sandbox::SandboxEngine::policy_to_nono_capabilities(*sandbox_policy, prebuilt_caps);
        }
    }

    pid_t pid = fork();

    if (pid < 0) {
        if (prebuilt_caps) nono_capability_set_free(prebuilt_caps);
        throw std::runtime_error("Failed to fork process");
    }

    if (pid == 0) {
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        if (should_sanitize) {
            clearenv();
            for (const auto& [k, v] : sanitized_env) {
                setenv(k.c_str(), v.c_str(), 1);
            }
        } else {
            for (const auto& [k, v] : env_vars) {
                setenv(k.c_str(), v.c_str(), 1);
            }
        }

        if (!working_dir.empty()) {
            chdir(working_dir.c_str());
        }

        if (stdout_pipe[1] >= 0) {
            close(stdout_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            close(stdout_pipe[1]);
        } else if (!inherit_output) {
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null >= 0) {
                dup2(dev_null, STDOUT_FILENO);
                close(dev_null);
            }
        }

        if (stderr_pipe[1] >= 0) {
            close(stderr_pipe[0]);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stderr_pipe[1]);
        } else if (!inherit_output) {
            int dev_null = open("/dev/null", O_WRONLY);
            if (dev_null >= 0) {
                dup2(dev_null, STDERR_FILENO);
                close(dev_null);
            }
        }

#ifdef HAVE_LIBCAP
        preserve_capabilities_for_exec();
#endif

        if (prebuilt_caps != nullptr) {
            nono_status status = nono_sandbox_apply(prebuilt_caps);
            if (status != NONO_OK) {
                bool is_enforced = (sandbox_policy.has_value() &&
                                    sandbox_policy->mode == lemon::sandbox::SandboxMode::Enforced);
                if (is_enforced || status != NONO_ERROR_UNSUPPORTED) {
                    _exit(127);
                }
            }
        }

        std::vector<char*> argv_ptrs;
        argv_ptrs.push_back(const_cast<char*>(executable.c_str()));
        for (const auto& arg : args) {
            argv_ptrs.push_back(const_cast<char*>(arg.c_str()));
        }
        argv_ptrs.push_back(nullptr);

        execvp(executable.c_str(), argv_ptrs.data());
        _exit(1);
    }

    if (prebuilt_caps != nullptr) {
        nono_capability_set_free(prebuilt_caps);
    }

    return pid;
}

std::unique_ptr<ProcessPlatform> create_process_platform() {
    return std::make_unique<LinuxProcessPlatform>();
}

} // namespace lemon::utils
