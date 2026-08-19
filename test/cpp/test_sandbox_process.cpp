#include <lemon/sandbox/env_scrubber.h>
#include <lemon/sandbox/sandbox_engine.h>
#include <lemon/sandbox/sandbox_policy.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/process_platform.h>
#include <lemon/wrapped_server.h>
#include "sandbox_test_utils.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using lemon::WrappedServer;
using lemon::sandbox::EnvScrubber;
using lemon::sandbox::NetworkAccess;
using lemon::sandbox::PathGrant;
using lemon::sandbox::PolicyPresets;
using lemon::sandbox::SandboxEngine;
using lemon::sandbox::SandboxMode;
using lemon::sandbox::SandboxPolicy;
using lemon::test::read_file_content;
using lemon::test::TempSandboxFixture;
using lemon::test::TestResult;
using lemon::utils::ProcessHandle;
using lemon::utils::ProcessManager;

int main() {
    TestResult r;
    TempSandboxFixture fixture("lemonade_proc_sb_test_");

    {
#ifndef _WIN32
        ProcessHandle h1 = ProcessManager::start_process(
            "/bin/sh", {"-c", "exit 0"}, "", false, false, {});
        r.check(h1.pid > 0, "start_process returns valid pid for baseline spawn");
        int exit1 = ProcessManager::wait_for_exit(h1, 5);
        r.check(exit1 == 0, "baseline spawn exits with code 0");

        SandboxPolicy policy;
        policy.add_read_path("/bin")
              .add_read_path("/usr/bin")
              .add_read_path("/lib")
              .add_read_path("/usr/lib")
              .add_read_path("/lib64")
              .add_read_path("/usr/lib64")
              .set_mode(SandboxMode::Auto);

        ProcessHandle h2 = ProcessManager::start_process(
            "/bin/sh", {"-c", "exit 42"}, "", false, false, {}, policy);
        r.check(h2.pid > 0, "start_process returns valid pid with SandboxPolicy");
        int exit2 = ProcessManager::wait_for_exit(h2, 5);
        r.check(exit2 == 42, "spawn with SandboxPolicy preserves exit code 42");

        ProcessHandle h3 = ProcessManager::start_process(
            "/bin/sh", {"-c", "sleep 30"}, "", false, false, {}, policy);
        r.check(h3.pid > 0, "start_process spawned sleeping child");
        r.check(ProcessManager::is_running(h3), "is_running reports true for active child");
        ProcessManager::stop_process(h3);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        r.check(!ProcessManager::is_running(h3), "is_running reports false after stop_process");
#else
        ProcessHandle h1 = ProcessManager::start_process(
            "cmd.exe", {"/c", "exit 0"}, "", false, false, {});
        r.check(h1.pid > 0 || h1.handle != nullptr, "Windows start_process returns handle");
        int exit1 = ProcessManager::wait_for_exit(h1, 5);
        r.check(exit1 == 0, "Windows baseline spawn exits with code 0");
#endif
    }

    {
#ifndef _WIN32
        setenv("LEMONADE_ADMIN_API_KEY", "super_secret_admin_key_123", 1);
        setenv("LEMONADE_API_KEY", "lemon_api_key_456", 1);
        setenv("OPENAI_API_KEY", "sk-proj-secret-openai-789", 1);
        setenv("AWS_SECRET_ACCESS_KEY", "aws_secret_key_abc", 1);
        setenv("HF_TOKEN", "hf_token_secret_xyz", 1);
        setenv("CUDA_VISIBLE_DEVICES", "0,1", 1);

        fs::path env_dump_file = fixture.root() / "env_dump.txt";
        std::string cmd = "env > \"" + env_dump_file.string() + "\"";
        SandboxPolicy policy;
        for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
            policy.path_grants.push_back(sp);
        }
        policy.add_write_path(fixture.root().string()).set_mode(SandboxMode::Auto);

        ProcessHandle h_env = ProcessManager::start_process(
            "/bin/sh", {"-c", cmd}, "", false, false, {}, policy);
        ProcessManager::wait_for_exit(h_env, 5);

        std::string dump_content = read_file_content(env_dump_file);

        r.check(dump_content.find("super_secret_admin_key_123") == std::string::npos,
                "child environment stripped LEMONADE_ADMIN_API_KEY");
        r.check(dump_content.find("lemon_api_key_456") == std::string::npos,
                "child environment stripped LEMONADE_API_KEY");
        r.check(dump_content.find("sk-proj-secret-openai-789") == std::string::npos,
                "child environment stripped OPENAI_API_KEY");
        r.check(dump_content.find("aws_secret_key_abc") == std::string::npos,
                "child environment stripped AWS_SECRET_ACCESS_KEY");
        r.check(dump_content.find("hf_token_secret_xyz") == std::string::npos,
                "child environment stripped HF_TOKEN");
        r.check(dump_content.find("CUDA_VISIBLE_DEVICES=0,1") != std::string::npos,
                "child environment preserved allowlisted CUDA_VISIBLE_DEVICES");
        r.check(dump_content.find("PATH=") != std::string::npos,
                "child environment preserved PATH");

        fs::path custom_dump_file = fixture.root() / "custom_env_dump.txt";
        std::string custom_cmd = "env > \"" + custom_dump_file.string() + "\"";
        std::vector<std::pair<std::string, std::string>> custom_env = {
            {"LEMONADE_DYNAMIC_TOKEN", "leak_me_not"},
            {"MY_EXPLICIT_VAR", "preserved_value"}
        };

        ProcessHandle h_custom = ProcessManager::start_process(
            "/bin/sh", {"-c", custom_cmd}, "", false, false, custom_env, policy);
        ProcessManager::wait_for_exit(h_custom, 5);

        std::string custom_content = read_file_content(custom_dump_file);

        r.check(custom_content.find("leak_me_not") == std::string::npos,
                "explicit custom LEMONADE_* variable stripped from child");
        r.check(custom_content.find("MY_EXPLICIT_VAR=preserved_value") != std::string::npos,
                "explicit custom non-sensitive variable preserved in child");
#endif
    }

    {
        std::string model_path = fixture.hf_model().string();
        SandboxPolicy policy = WrappedServer::build_default_sandbox_policy(
            model_path, "/usr/bin/llama-server", 8080, "vulkan");

        r.check(policy.bind_port == 8080, "policy bind_port set to 8080");
        r.check(policy.network_access == NetworkAccess::LoopbackOnly, "network_access is LoopbackOnly");
        r.check(policy.has_read_path("/usr/bin/llama-server"), "executable read grant added");
#if !defined(_WIN32) && !defined(__APPLE__)
        r.check(policy.has_device("/dev/dri"), "vulkan backend includes /dev/dri device grant");
#endif

        r.check(!policy.has_read_path(fixture.hf_token().string()), "token file path not in grants");
        r.check(!policy.has_read_path(fixture.hf_home().string()), "cache root directory not in grants");

        SandboxPolicy rocm_policy = WrappedServer::build_default_sandbox_policy(
            model_path, "/usr/bin/llama-server", 8080, "rocm");
#if !defined(_WIN32) && !defined(__APPLE__)
        r.check(rocm_policy.has_device("/dev/kfd"), "rocm backend includes /dev/kfd");
        r.check(rocm_policy.has_device("/dev/dri"), "rocm backend includes /dev/dri");
#endif

        SandboxPolicy npu_policy = WrappedServer::build_default_sandbox_policy(
            model_path, "/usr/bin/flm", 8081, "npu");
#if !defined(_WIN32) && !defined(__APPLE__)
        r.check(npu_policy.has_device("/dev/accel"), "npu backend includes /dev/accel");
        r.check(npu_policy.has_device("/dev/amdxdna"), "npu backend includes /dev/amdxdna");
#endif

        SandboxPolicy cpu_policy = WrappedServer::build_default_sandbox_policy(
            model_path, "/usr/bin/koko", 8082, "cpu");
#if !defined(_WIN32) && !defined(__APPLE__)
        r.check(!cpu_policy.has_device("/dev/kfd"), "cpu backend omits /dev/kfd");
#endif
    }

    {
#ifndef _WIN32
        SandboxPolicy scrubbed_policy;
        scrubbed_policy.set_mode(SandboxMode::ScrubbedOnly);
        ProcessHandle h_scrub = ProcessManager::start_process(
            "/bin/sh", {"-c", "exit 0"}, "", false, false, {}, scrubbed_policy);
        r.check(h_scrub.pid > 0, "ScrubbedOnly mode spawns cleanly");
        r.check(ProcessManager::wait_for_exit(h_scrub, 5) == 0, "ScrubbedOnly exits 0");

        SandboxPolicy auto_policy;
        auto_policy.set_mode(SandboxMode::Auto);
        auto_policy.add_read_path("/bin")
                   .add_read_path("/usr/bin")
                   .add_read_path("/lib")
                   .add_read_path("/usr/lib")
                   .add_read_path("/lib64")
                   .add_read_path("/usr/lib64");
        ProcessHandle h_auto = ProcessManager::start_process(
            "/bin/sh", {"-c", "exit 0"}, "", false, false, {}, auto_policy);
        r.check(h_auto.pid > 0, "Auto mode spawns cleanly");
        r.check(ProcessManager::wait_for_exit(h_auto, 5) == 0, "Auto mode exits 0");

        auto engine = SandboxEngine::create_for_platform();
        if (engine && engine->is_kernel_enforced()) {
            SandboxPolicy enforced_policy;
            enforced_policy.set_mode(SandboxMode::Enforced);
            for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
                enforced_policy.path_grants.push_back(sp);
            }
            enforced_policy.add_read_path("/bin")
                           .add_write_path("/dev")
                           .add_read_path(fixture.allowed_dir().string());

            std::string containment_script =
                "cat \"" + fixture.allowed_file().string() + "\" >/dev/null 2>&1 || exit 10; "
                "if cat \"" + fixture.forbidden_canary().string() + "\" >/dev/null 2>&1; then "
                "  exit 20; "
                "else "
                "  exit 0; "
                "fi";

            ProcessHandle h_enf = ProcessManager::start_process(
                "/bin/sh", {"-c", containment_script}, "", false, false, {}, enforced_policy);
            r.check(h_enf.pid > 0, "Enforced mode spawned containment test child");
            int enf_exit = ProcessManager::wait_for_exit(h_enf, 5);

            r.check(enf_exit == 0, "Enforced sandbox successfully blocked unauthorized file access (exit 0)",
                    "Child exit code was " + std::to_string(enf_exit));
        } else {
            std::cout << "  [SKIP] Kernel containment enforcement not supported on this platform/engine." << std::endl;
        }

        // Test lemonade-sandbox-exec helper executable functionally and qualitatively
        std::string helper_path = "./lemonade-sandbox-exec";
        if (fs::exists(helper_path)) {
            SandboxPolicy policy;
            policy.set_mode(SandboxMode::ScrubbedOnly);
            nlohmann::json j = policy;
            std::string policy_json = j.dump();

            ProcessHandle h_exec = ProcessManager::start_process(
                helper_path, {"--policy", policy_json, "--", "/bin/sh", "-c", "exit 0"}, "", false, false, {});
            r.check(h_exec.pid > 0, "lemonade-sandbox-exec spawned helper process");
            int exec_exit = ProcessManager::wait_for_exit(h_exec, 5);
            r.check(exec_exit == 0, "lemonade-sandbox-exec successfully executed target (exit 0)",
                    "Child exit code was " + std::to_string(exec_exit));

            // Test policy validation failure in lemonade-sandbox-exec (governance gate)
            SandboxPolicy bad_policy;
            bad_policy.allow_env_var("LEMONADE_API_KEY");
            nlohmann::json bad_j = bad_policy;
            ProcessHandle h_bad = ProcessManager::start_process(
                helper_path, {"--policy", bad_j.dump(), "--", "/bin/sh", "-c", "exit 0"}, "", false, false, {});
            r.check(h_bad.pid > 0, "lemonade-sandbox-exec spawned helper with bad policy");
            int bad_exit = ProcessManager::wait_for_exit(h_bad, 5);
            r.check(bad_exit != 0, "lemonade-sandbox-exec rejected invalid policy (non-zero exit)",
                    "Child exit code was " + std::to_string(bad_exit));
        }
#endif
    }

    r.report_summary("ProcessSandbox");
    return r.exit_code();
}
