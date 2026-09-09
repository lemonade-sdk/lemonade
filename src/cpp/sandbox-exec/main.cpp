#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "lemon/sandbox/sandbox_policy.h"
#include "lemon/sandbox/nono_ffi.h"
#include <nlohmann/json.hpp>

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: lemonade-sandbox-exec --policy <json_or_file> -- <executable> [args...]" << std::endl;
        return 1;
    }

    std::string policy_input;
    std::string target_executable;
    std::vector<std::string> target_args;

    int idx = 1;
    while (idx < argc) {
        std::string arg = argv[idx];
        if (arg == "--policy" && idx + 1 < argc) {
            std::string val = argv[++idx];
            if (val == "-") {
                std::ostringstream ss;
                ss << std::cin.rdbuf();
                policy_input = ss.str();
            } else {
                policy_input = val;
            }
        } else if (arg == "--policy-file" && idx + 1 < argc) {
            std::ifstream file(argv[++idx]);
            if (!file) {
                std::cerr << "lemonade-sandbox-exec: failed to open policy file: " << argv[idx] << std::endl;
                return 1;
            }
            std::ostringstream ss;
            ss << file.rdbuf();
            policy_input = ss.str();
        } else if (arg == "--") {
            ++idx;
            if (idx < argc) {
                target_executable = argv[idx++];
                target_args.push_back(target_executable);
                while (idx < argc) {
                    target_args.push_back(argv[idx++]);
                }
            }
            break;
        }
        ++idx;
    }

    if (policy_input.empty() || target_executable.empty()) {
        std::cerr << "lemonade-sandbox-exec: missing policy or target executable" << std::endl;
        return 1;
    }

    lemon::sandbox::SandboxPolicy policy;
    try {
        nlohmann::json j = nlohmann::json::parse(policy_input);
        policy = j.get<lemon::sandbox::SandboxPolicy>();
    } catch (const std::exception& e) {
        std::cerr << "lemonade-sandbox-exec: invalid policy JSON: " << e.what() << std::endl;
        return 1;
    }

    std::string val_err;
    if (!lemon::sandbox::validate_policy(policy, &val_err)) {
        std::cerr << "lemonade-sandbox-exec: policy validation failed: " << val_err << std::endl;
        return 1;
    }

    if (policy.mode != lemon::sandbox::SandboxMode::Disabled &&
        policy.mode != lemon::sandbox::SandboxMode::ScrubbedOnly) {

        nono_capability_set* caps = nono_capability_set_new();
        if (!caps) {
            std::cerr << "lemonade-sandbox-exec: failed to allocate capability set" << std::endl;
            return 1;
        }

        for (const auto& pg : policy.path_grants) {
            if (pg.write_allowed) {
                nono_capability_add_fs_write(caps, pg.path.c_str());
            } else {
                nono_capability_add_fs_read(caps, pg.path.c_str());
            }
        }

        for (const auto& dg : policy.device_grants) {
            nono_capability_add_device(caps, dg.c_str());
        }

        if (policy.network_access == lemon::sandbox::NetworkAccess::DenyAll) {
            nono_capability_set_network_egress(caps, false);
            nono_capability_set_network_loopback(caps, false);
        } else if (policy.network_access == lemon::sandbox::NetworkAccess::LoopbackOnly) {
            nono_capability_set_network_egress(caps, false);
            nono_capability_set_network_loopback(caps, true);
        } else if (policy.network_access == lemon::sandbox::NetworkAccess::Full) {
            nono_capability_set_network_egress(caps, true);
            nono_capability_set_network_loopback(caps, true);
        }

        if (policy.bind_port > 0) {
            nono_capability_set_bind_port(caps, policy.bind_port);
        }

        nono_status status = nono_sandbox_apply(caps);
        nono_capability_set_free(caps);

        if (status != NONO_OK) {
            const char* err = nono_get_last_error();
            std::cerr << "lemonade-sandbox-exec: failed to apply sandbox: "
                      << (err ? err : "unknown error") << std::endl;
            return 1;
        }
    }

#ifndef _WIN32
    std::vector<char*> exec_argv;
    exec_argv.reserve(target_args.size() + 1);
    for (auto& s : target_args) {
        exec_argv.push_back(const_cast<char*>(s.c_str()));
    }
    exec_argv.push_back(nullptr);

    execv(target_executable.c_str(), exec_argv.data());

    std::cerr << "lemonade-sandbox-exec: execv failed for " << target_executable
              << ": " << std::strerror(errno) << std::endl;
    return 127;
#else
    std::cerr << "lemonade-sandbox-exec: POSIX exec not supported on Windows" << std::endl;
    return 1;
#endif
}
