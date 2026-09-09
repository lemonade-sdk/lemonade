#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "lemon/sandbox/env_scrubber.h"
#include "lemon/sandbox/sandbox_engine.h"
#include "lemon/sandbox/sandbox_policy.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/process_platform.h"
#include "sandbox_test_utils.h"

namespace fs = std::filesystem;
using lemon::sandbox::EnvScrubber;
using lemon::sandbox::NetworkAccess;
using lemon::sandbox::PolicyPresets;
using lemon::sandbox::SandboxEngine;
using lemon::sandbox::SandboxMode;
using lemon::sandbox::SandboxPolicy;
using lemon::test::parse_env_lines;
using lemon::test::read_file_content;
using lemon::test::read_file_lines;
using lemon::test::TempSandboxFixture;
using lemon::test::TestResult;
using lemon::utils::ProcessHandle;
using lemon::utils::ProcessManager;

int main() {
    TestResult overall;
    TempSandboxFixture fixture("lemonade_test_scrub_");

    {
        TestResult r;

#ifndef _WIN32
        struct SecretEntry {
            std::string key;
            std::string canary_value;
            std::string category;
        };

        std::vector<SecretEntry> poison_secrets = {
            {"LEMONADE_ADMIN_API_KEY", "canary_admin_key_999888777", "LEMONADE_ prefix"},
            {"LEMONADE_API_KEY", "canary_user_api_key_111222333", "LEMONADE_ prefix"},
            {"LEMONADE_ROUTER_TOKEN", "canary_router_secret_aaaabbbb", "LEMONADE_ prefix"},
            {"LEMONADE_SESSION_COOKIE", "canary_cookie_deadbeef", "LEMONADE_ prefix"},
            {"LEMONADE_INTERNAL_KEY", "canary_internal_pass_123", "LEMONADE_ prefix"},
            {"LEMONADE_CLOUD_AUTH_BEARER", "canary_cloud_bearer_token", "LEMONADE_ prefix"},

            {"AWS_ACCESS_KEY_ID", "AKIAIOSFODNN7EXAMPLE_CANARY", "AWS_ prefix"},
            {"AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY_CANARY", "AWS_ prefix"},
            {"AWS_SESSION_TOKEN", "AQoDYXdzEJr111111111111111EXAMPLE_CANARY", "AWS_ prefix"},
            {"AWS_SECURITY_TOKEN", "AQoDYXdzEJr2222222222222EXAMPLE_CANARY", "AWS_ prefix"},

            {"AZURE_CLIENT_SECRET", "azure_sec_canary_333444555", "AZURE_ prefix"},
            {"AZURE_TENANT_ID", "azure_tenant_canary_666777888", "AZURE_ prefix"},
            {"AZURE_CLIENT_ID", "azure_client_id_canary_999", "AZURE_ prefix"},
            {"AZURE_OPENAI_API_KEY", "azure_openai_canary_sk_123", "AZURE_ prefix"},

            {"OPENAI_API_KEY", "sk-proj-canary-openai-secret-token-12345", "Exact Provider"},
            {"ANTHROPIC_API_KEY", "sk-ant-canary-anthropic-key-67890", "Exact Provider"},
            {"FIREWORKS_API_KEY", "fw-canary-key-abcdef", "Exact Provider"},
            {"COHERE_API_KEY", "co-canary-key-123456", "Exact Provider"},
            {"CO_API_KEY", "co-short-canary-789012", "Exact Provider"},
            {"GROQ_API_KEY", "gsk_canary_groq_key_987654", "Exact Provider"},
            {"MISTRAL_API_KEY", "mis_canary_mistral_key_456789", "Exact Provider"},
            {"DEEPSEEK_API_KEY", "sk-deepseek-canary-321654", "Exact Provider"},
            {"GEMINI_API_KEY", "AIzaSyCanaryGeminiKey_123456789", "Exact Provider"},
            {"GOOGLE_API_KEY", "AIzaSyCanaryGoogleKey_987654321", "Exact Provider"},
            {"PALM_API_KEY", "AIzaSyCanaryPalmKey_abcdef", "Exact Provider"},
            {"XAI_API_KEY", "xai-canary-key-11223344", "Exact Provider"},
            {"TOGETHER_API_KEY", "together_canary_key_556677", "Exact Provider"},
            {"TOGETHERAI_API_KEY", "togetherai_canary_key_889900", "Exact Provider"},
            {"CEREBRAS_API_KEY", "csk-canary-cerebras-key-123", "Exact Provider"},
            {"PERPLEXITY_API_KEY", "pplx-canary-perplexity-key-456", "Exact Provider"},
            {"PPLX_API_KEY", "pplx-canary-short-key-789", "Exact Provider"},
            {"SAMBANOVA_API_KEY", "samba-canary-key-abc", "Exact Provider"},
            {"CHROMA_API_KEY", "chroma-canary-key-def", "Exact Provider"},
            {"CHROMA_SERVER_AUTH_CREDENTIALS", "chroma-auth-canary-ghi", "Exact Provider"},
            {"OPENROUTER_API_KEY", "sk-or-canary-openrouter-key-jkl", "Exact Provider"},
            {"VOYAGE_API_KEY", "voyage-canary-key-mno", "Exact Provider"},
            {"REPLICATE_API_TOKEN", "r8_canary_replicate_token_pqr", "Exact Provider"},
            {"ANYSCALE_API_KEY", "ese_canary_anyscale_key_stu", "Exact Provider"},
            {"AI21_API_KEY", "ai21_canary_key_vwx", "Exact Provider"},
            {"OCTOAI_API_KEY", "octo_canary_key_yz0", "Exact Provider"},
            {"NOVITA_API_KEY", "novita_canary_key_123", "Exact Provider"},
            {"RUNPOD_API_KEY", "runpod_canary_key_456", "Exact Provider"},
            {"FAL_KEY", "fal_canary_key_789:abc", "Exact Provider"},
            {"CLOUDFLARE_API_TOKEN", "cf_canary_token_def", "Exact Provider"},

            {"HF_TOKEN", "hf_canary_secret_token_alpha", "HF Token"},
            {"HUGGING_FACE_HUB_TOKEN", "hf_hub_canary_token_beta", "HF Token"},
            {"HF_API_TOKEN", "hf_api_canary_token_gamma", "HF Token"},
            {"HUGGINGFACE_TOKEN", "hf_legacy_canary_token_delta", "HF Token"},
            {"HF_TOKEN_PATH", "/tmp/forbidden/hf_token_path_canary", "HF Token"},
            {"MODELSCOPE_API_TOKEN", "ms_canary_token_epsilon", "Hub Token"},
            {"KAGGLE_KEY", "kaggle_key_canary_zeta", "Hub Token"},
            {"KAGGLE_USERNAME", "kaggle_user_canary_eta", "Hub Token"},

            {"GITHUB_TOKEN", "ghp_canary_github_personal_access_token_123", "VCS Token"},
            {"GH_TOKEN", "gho_canary_github_oauth_token_456", "VCS Token"},
            {"GITLAB_TOKEN", "glpat-canary_gitlab_token_789", "VCS Token"},
            {"BITBUCKET_TOKEN", "bb_canary_token_012", "VCS Token"},
            {"WANDB_API_KEY", "wandb_canary_key_345", "Tracking Token"},
            {"COMET_API_KEY", "comet_canary_key_678", "Tracking Token"},
            {"LANGCHAIN_API_KEY", "lsv2_canary_langchain_key_901", "Tracking Token"},
            {"LANGSMITH_API_KEY", "lsv2_canary_langsmith_key_234", "Tracking Token"},

            {"GOOGLE_APPLICATION_CREDENTIALS", "/tmp/canary_gcp_service_account.json", "System Cred"},
            {"SSH_AUTH_SOCK", "/tmp/canary_ssh_auth_sock_9999", "SSH Agent"},
            {"SSH_AGENT_PID", "99999", "SSH Agent"},
            {"GPG_AGENT_INFO", "/tmp/canary_gpg_agent_sock:1111:1", "GPG Agent"},

            {"MY_CUSTOM_BACKEND_API_KEY", "canary_custom_api_key_val", "Suffix Match"},
            {"INFERENCE_ENGINE_API_TOKEN", "canary_engine_api_token_val", "Suffix Match"},
            {"PRIMARY_DATABASE_SECRET_KEY", "canary_db_secret_key_val", "Suffix Match"},
            {"BACKUP_AUTH_SECRET_TOKEN", "canary_backup_secret_token_val", "Suffix Match"},
            {"INTERNAL_BLOB_STORAGE_ACCESS_KEY", "canary_storage_access_key_val", "Suffix Match"},
            {"OIDC_CLIENT_AUTH_TOKEN", "canary_oidc_auth_token_val", "Suffix Match"},
            {"OAUTH_USER_BEARER_TOKEN", "canary_oauth_bearer_token_val", "Suffix Match"},
            {"HTTPS_SERVER_PRIVATE_KEY", "canary_https_private_key_val", "Suffix Match"},

            {"lemonade_admin_api_key", "canary_lowercase_admin_key", "Case Variation"},
            {"OpenAI_Api_Key", "canary_mixedcase_openai_key", "Case Variation"},
            {"hf_token", "canary_lowercase_hf_token", "Case Variation"},
            {"Aws_Secret_Access_Key", "canary_mixedcase_aws_secret", "Case Variation"},
            {"Google_Application_Credentials", "canary_mixedcase_gcp_creds", "Case Variation"}
        };

        for (const auto& sec : poison_secrets) {
            setenv(sec.key.c_str(), sec.canary_value.c_str(), 1);
        }

        fs::path dump_file = fixture.root() / "ambient_env_dump.txt";
        std::string cmd = "env > \"" + dump_file.string() + "\"";

        SandboxPolicy p;
        p.set_mode(SandboxMode::ScrubbedOnly);

        ProcessHandle handle = ProcessManager::start_process(
            "/bin/sh", {"-c", cmd}, "", false, false, {}, p);

        r.check(handle.pid > 0, "Spawning test process succeeded under ambient secret poisoning");
        int exit_code = ProcessManager::wait_for_exit(handle, 5);
        r.check(exit_code == 0, "Child process exited cleanly (exit code 0)");

        std::string child_dump = read_file_content(dump_file);
        r.check(!child_dump.empty(), "Child process dumped non-empty environment");

        int leaked_count = 0;
        for (const auto& sec : poison_secrets) {
            if (child_dump.find(sec.key + "=") != std::string::npos ||
                child_dump.find(sec.canary_value) != std::string::npos) {
                std::printf("  [SECURITY LEAK] Found secret '%s' in child environment!\n", sec.key.c_str());
                ++leaked_count;
            }
        }
        r.check(leaked_count == 0, "0 ambient secrets leaked to child process",
                "Leaked count: " + std::to_string(leaked_count));

        bool has_path = (child_dump.find("PATH=") != std::string::npos);
        r.check(has_path, "Standard PATH variable was preserved");

        // Clean up environment variables
        for (const auto& sec : poison_secrets) {
            unsetenv(sec.key.c_str());
        }
#else
        r.check(true, "Windows test suite fallback executed");
#endif

        r.report_summary("Ambient Secret Injection & Zero Leakage");
        overall.passed += r.passed.load();
        overall.failed += r.failed.load();
    }

    {
        TestResult r;

#ifndef _WIN32
        setenv("CUDA_VISIBLE_DEVICES", "0,1,3", 1);
        setenv("HIP_VISIBLE_DEVICES", "1,2", 1);
        setenv("ROCM_PATH", "/opt/rocm-custom-version", 1);
        setenv("PYTHONPATH", "/opt/lemonade/custom_python_modules", 1);
        setenv("ESPEAK_DATA_PATH", "/usr/share/custom_espeak", 1);
        setenv("OMP_NUM_THREADS", "8", 1);
        setenv("USER", "lemonade_test_runner", 1);
        setenv("LANG", "en_US.UTF-8", 1);
        setenv("MY_RANDOM_UNALLOWED_VAR", "drop_me_please", 1);

        SandboxPolicy policy1;
        for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
            policy1.path_grants.push_back(sp);
        }
        policy1.allow_env_vars({
            "CUDA_VISIBLE_DEVICES", "HIP_VISIBLE_DEVICES", "ROCM_PATH",
            "PYTHONPATH", "ESPEAK_DATA_PATH", "OMP_NUM_THREADS", "USER", "LANG", "PATH"
        });
        policy1.add_write_path(fixture.root().string()).set_mode(SandboxMode::Auto);

        fs::path dump = fixture.root() / "allowlist_dump.txt";
        std::string cmd = "env > \"" + dump.string() + "\"";

        ProcessHandle h1 = ProcessManager::start_process(
            "/bin/sh", {"-c", cmd}, "", false, false, {}, policy1);
        ProcessManager::wait_for_exit(h1, 5);

        auto env_map = parse_env_lines(read_file_lines(dump));

        r.check(env_map["CUDA_VISIBLE_DEVICES"] == "0,1,3", "CUDA_VISIBLE_DEVICES preserved exactly: 0,1,3");
        r.check(env_map["HIP_VISIBLE_DEVICES"] == "1,2", "HIP_VISIBLE_DEVICES preserved exactly: 1,2");
        r.check(env_map["ROCM_PATH"] == "/opt/rocm-custom-version", "ROCM_PATH preserved exactly");
        r.check(env_map["PYTHONPATH"] == "/opt/lemonade/custom_python_modules", "PYTHONPATH preserved exactly");
        r.check(env_map["ESPEAK_DATA_PATH"] == "/usr/share/custom_espeak", "ESPEAK_DATA_PATH preserved exactly");
        r.check(env_map["OMP_NUM_THREADS"] == "8", "OMP_NUM_THREADS preserved exactly: 8");
        r.check(env_map["USER"] == "lemonade_test_runner", "USER preserved exactly");
        r.check(env_map["LANG"] == "en_US.UTF-8", "LANG preserved exactly");
        r.check(!env_map["PATH"].empty(), "PATH is non-empty and present in child");
        r.check(env_map.find("MY_RANDOM_UNALLOWED_VAR") == env_map.end(),
                "Non-allowlisted ambient variable was correctly omitted");

        fs::path custom_dump = fixture.root() / "custom_env_dump.txt";
        std::string custom_cmd = "env > \"" + custom_dump.string() + "\"";

        std::vector<std::pair<std::string, std::string>> custom_env = {
            {"BACKEND_CUSTOM_PORT", "8999"},
            {"MODEL_CONTEXT_WINDOW", "32768"},
            {"LEMONADE_MALICIOUS_CUSTOM_INJECTION", "secret_admin_token_injected"},
            {"VENDOR_INJECTED_API_KEY", "secret_vendor_key_injected"}
        };

        ProcessHandle h2 = ProcessManager::start_process(
            "/bin/sh", {"-c", custom_cmd}, "", false, false, custom_env, policy1);
        ProcessManager::wait_for_exit(h2, 5);

        auto custom_map = parse_env_lines(read_file_lines(custom_dump));

        r.check(custom_map["BACKEND_CUSTOM_PORT"] == "8999", "Custom non-sensitive var BACKEND_CUSTOM_PORT passed");
        r.check(custom_map["MODEL_CONTEXT_WINDOW"] == "32768", "Custom non-sensitive var MODEL_CONTEXT_WINDOW passed");
        r.check(custom_map.find("LEMONADE_MALICIOUS_CUSTOM_INJECTION") == custom_map.end(),
                "Explicit sensitive LEMONADE_* custom var was stripped by EnvScrubber");
        r.check(custom_map.find("VENDOR_INJECTED_API_KEY") == custom_map.end(),
                "Explicit sensitive *_API_KEY custom var was stripped by EnvScrubber");

        fs::path extra_dump = fixture.root() / "extra_allowlist_dump.txt";
        std::string extra_cmd = "env > \"" + extra_dump.string() + "\"";

        setenv("MY_SPECIAL_AGENT_ENV", "special_agent_value_123", 1);

        SandboxPolicy extra_policy;
        for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
            extra_policy.path_grants.push_back(sp);
        }
        extra_policy.add_read_path("/bin")
                    .add_read_path("/usr/bin")
                    .add_write_path(fixture.root().string())
                    .allow_env_var("MY_SPECIAL_AGENT_ENV")
                    .set_env_var("POLICY_EXPLICIT_VAR", "explicit_policy_val_456");

        ProcessHandle h3 = ProcessManager::start_process(
            "/bin/sh", {"-c", extra_cmd}, "", false, false, {}, extra_policy);
        ProcessManager::wait_for_exit(h3, 5);

        auto extra_map = parse_env_lines(read_file_lines(extra_dump));

        r.check(extra_map["MY_SPECIAL_AGENT_ENV"] == "special_agent_value_123",
                "Extra allowlisted ambient var MY_SPECIAL_AGENT_ENV preserved via SandboxPolicy");
        r.check(extra_map["POLICY_EXPLICIT_VAR"] == "explicit_policy_val_456",
                "Policy explicit env var POLICY_EXPLICIT_VAR passed to child");

#else
        r.check(true, "Windows allowlist test fallback executed");
#endif

        r.report_summary("Allowlisted Environment Preservation");
        overall.passed += r.passed.load();
        overall.failed += r.failed.load();
    }

    {
        TestResult r;

#ifndef _WIN32
        const int NUM_THREADS = 24;
        const int ITERATIONS_PER_THREAD = 10;
        std::atomic<int> total_spawns{0};
        std::atomic<int> concurrent_leaks{0};
        std::atomic<int> failed_exits{0};

        std::vector<std::thread> workers;
        workers.reserve(NUM_THREADS);

        for (int t = 0; t < NUM_THREADS; ++t) {
            workers.emplace_back([&, t]() {
                for (int iter = 0; iter < ITERATIONS_PER_THREAD; ++iter) {
                    fs::path out_file = fixture.root() / (
                        "conc_dump_t" + std::to_string(t) + "_i" + std::to_string(iter) + ".txt");
                    std::string cmd = "env > \"" + out_file.string() + "\"";

                    std::vector<std::pair<std::string, std::string>> custom_env = {
                        {"LEMONADE_THREAD_SECRET_" + std::to_string(t), "canary_thr_sec_" + std::to_string(iter)},
                        {"THREAD_" + std::to_string(t) + "_VENDOR_API_KEY", "canary_vkey_" + std::to_string(iter)},
                        {"VALID_CUSTOM_THREAD_VAR", "valid_thread_val_" + std::to_string(t)}
                    };

                    SandboxPolicy p;
                    if (iter % 2 == 0) {
                        p.set_mode(SandboxMode::ScrubbedOnly);
                    } else {
                        p.set_mode(SandboxMode::Auto);
                        for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
                            p.path_grants.push_back(sp);
                        }
                        p.add_read_path("/bin")
                         .add_read_path("/usr/bin")
                         .add_write_path(fixture.root().string());
                    }

                    ProcessHandle h = ProcessManager::start_process(
                        "/bin/sh", {"-c", cmd}, "", false, false, custom_env, p);

                    if (h.pid <= 0) {
                        failed_exits.fetch_add(1);
                        continue;
                    }
                    total_spawns.fetch_add(1);

                    int exit_code = ProcessManager::wait_for_exit(h, 10);
                    if (exit_code != 0) {
                        failed_exits.fetch_add(1);
                    }

                    std::string dump_text = read_file_content(out_file);
                    if (dump_text.find("canary_admin_key") != std::string::npos ||
                        dump_text.find("canary_user_api_key") != std::string::npos ||
                        dump_text.find("sk-proj-canary-openai") != std::string::npos ||
                        dump_text.find("canary_thr_sec_") != std::string::npos ||
                        dump_text.find("canary_vkey_") != std::string::npos ||
                        dump_text.find("AKIAIOSFODNN7EXAMPLE_CANARY") != std::string::npos) {
                        concurrent_leaks.fetch_add(1);
                    }

                    if (dump_text.find("PATH=") == std::string::npos ||
                        dump_text.find("CUDA_VISIBLE_DEVICES=") == std::string::npos ||
                        dump_text.find("VALID_CUSTOM_THREAD_VAR=valid_thread_val_" + std::to_string(t)) == std::string::npos) {
                        failed_exits.fetch_add(1);
                    }
                }
            });
        }

        for (auto& w : workers) {
            w.join();
        }

        r.check(total_spawns.load() == NUM_THREADS * ITERATIONS_PER_THREAD,
                "Concurrent spawning completed all " + std::to_string(NUM_THREADS * ITERATIONS_PER_THREAD) + " child processes",
                "Spawns: " + std::to_string(total_spawns.load()));
        r.check(failed_exits.load() == 0,
                "0 child process execution failures during heavy concurrency stress",
                "Failed exits: " + std::to_string(failed_exits.load()));
        r.check(concurrent_leaks.load() == 0,
                "0 secret leaks detected across all concurrent child processes under active mutation",
                "Leaks: " + std::to_string(concurrent_leaks.load()));

#else
        r.check(true, "Windows concurrency test fallback executed");
#endif

        r.report_summary("Concurrency Stress Test");
        overall.passed += r.passed.load();
        overall.failed += r.failed.load();
    }

    {
        TestResult r;

#ifndef _WIN32
        try {
            ProcessHandle h_nonexist = ProcessManager::start_process(
                "/nonexistent/path/to/binary_12345", {"--arg"}, "", false, false, {});
            int exit_nonexist = ProcessManager::wait_for_exit(h_nonexist, 5);
            r.check(exit_nonexist != 0,
                    "Non-existent executable exits with non-zero exit code (" + std::to_string(exit_nonexist) + ")");
        } catch (...) {
            r.check(true, "Non-existent binary threw clean exception on spawn");
        }

        ProcessHandle h_sleep = ProcessManager::start_process(
            "/bin/sh", {"-c", "sleep 60"}, "", false, false, {});
        r.check(h_sleep.pid > 0, "Spawned long-running sleep child");
        r.check(ProcessManager::is_running(h_sleep), "is_running reports true for live child");

        ProcessManager::stop_process(h_sleep);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        r.check(!ProcessManager::is_running(h_sleep), "is_running reports false after stop_process");

        ProcessHandle h_stubborn = ProcessManager::start_process(
            "/bin/sh", {"-c", "trap '' TERM; sleep 60"}, "", false, false, {});
        r.check(h_stubborn.pid > 0, "Spawned SIGTERM-ignoring stubborn child");
        r.check(ProcessManager::is_running(h_stubborn), "Stubborn child is running");

        ProcessManager::kill_process(h_stubborn);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        r.check(!ProcessManager::is_running(h_stubborn), "kill_process (SIGKILL) successfully killed stubborn child");

        ProcessHandle h_quick = ProcessManager::start_process(
            "/bin/sh", {"-c", "exit 77"}, "", false, false, {});
        ProcessManager::wait_for_exit(h_quick, 5);
        int reaped_code = ProcessManager::reap_process(h_quick);
        r.check(reaped_code == 77 || reaped_code == -1,
                "reap_process cleaned up terminated child handle (reaped_code: " + std::to_string(reaped_code) + ")");

        int lines_seen = 0;
        int cancel_code = ProcessManager::run_process_with_output(
            "/bin/sh", {"-c", "for i in 1 2 3 4 5 6 7 8 9 10; do echo \"line $i\"; sleep 0.05; done"},
            [&lines_seen](const std::string& line) {
                ++lines_seen;
                if (lines_seen >= 3) {
                    return false;
                }
                return true;
            },
            "", 5, true);

        r.check(cancel_code == -1, "run_process_with_output returned -1 when terminated by callback");
        r.check(lines_seen == 3, "Callback stopped after seeing exactly 3 lines (saw " + std::to_string(lines_seen) + ")");

#else
        r.check(true, "Windows lifecycle test fallback executed");
#endif

        r.report_summary("Process Lifecycle Edge Cases & Robustness");
        overall.passed += r.passed.load();
        overall.failed += r.failed.load();
    }

    if (overall.failed.load() == 0) {
        return 0;
    } else {
        return 1;
    }
}
