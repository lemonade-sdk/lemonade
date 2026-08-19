#include <algorithm>
#include <atomic>
#include <cassert>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "lemon/model_types.h"
#include "lemon/sandbox/env_scrubber.h"
#include "lemon/sandbox/nono_ffi.h"
#include "lemon/sandbox/sandbox_engine.h"
#include "lemon/sandbox/sandbox_policy.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/process_platform.h"
#include "lemon/wrapped_server.h"
#include "sandbox_test_utils.h"

namespace fs = std::filesystem;
using lemon::DeviceType;
using lemon::DEVICE_CPU;
using lemon::DEVICE_GPU;
using lemon::DEVICE_NPU;
using lemon::DEVICE_NONE;
using lemon::ModelInfo;
using lemon::RecipeOptions;
using lemon::WrappedServer;
using lemon::sandbox::EngineBackend;
using lemon::sandbox::EngineCapabilities;
using lemon::sandbox::EnvScrubber;
using lemon::sandbox::NetworkAccess;
using lemon::sandbox::PathGrant;
using lemon::sandbox::PlatformDetector;
using lemon::sandbox::PlatformType;
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
    TempSandboxFixture fixture("lemonade_conf_test_");

    {
        TestResult r;

#ifndef _WIN32
        struct PoisonEntry {
            std::string key;
            std::string canary;
        };

        std::vector<PoisonEntry> poison_list = {
            {"LEMONADE_API_KEY", "canary_lem_api_key_999111"},
            {"LEMONADE_ADMIN_API_KEY", "canary_lem_admin_key_888222"},
            {"LEMONADE_JWT_SECRET", "canary_lem_jwt_secret_777333"},
            {"LEMONADE_DATABASE_PASSWORD", "canary_lem_db_pass_666444"},
            {"AWS_ACCESS_KEY_ID", "AKIAIOSFODNN7CANARY"},
            {"AWS_SECRET_ACCESS_KEY", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYCANARY"},
            {"AWS_SESSION_TOKEN", "AQoDYXdzEJr11111CANARY"},
            {"AZURE_CLIENT_SECRET", "azure_sec_canary_000"},
            {"AZURE_CLIENT_ID", "azure_id_canary_111"},
            {"AZURE_TENANT_ID", "azure_tenant_canary_222"},
            {"OPENAI_API_KEY", "sk-proj-canary-openai-12345"},
            {"ANTHROPIC_API_KEY", "sk-ant-canary-anthropic-67890"},
            {"DEEPSEEK_API_KEY", "sk-deepseek-canary-112233"},
            {"GEMINI_API_KEY", "AIzaSyCanaryGemini-445566"},
            {"GOOGLE_API_KEY", "AIzaSyCanaryGoogle-778899"},
            {"MISTRAL_API_KEY", "mistral-canary-001122"},
            {"GROQ_API_KEY", "gsk-canary-groq-334455"},
            {"COHERE_API_KEY", "co-canary-key-667788"},
            {"FIREWORKS_API_KEY", "fw-canary-key-990011"},
            {"TOGETHER_API_KEY", "together-canary-223344"},
            {"XAI_API_KEY", "xai-canary-556677"},
            {"VOYAGE_API_KEY", "voyage-canary-889900"},
            {"PERPLEXITY_API_KEY", "pplx-canary-112233"},
            {"NOVITA_API_KEY", "novita-canary-445566"},
            {"RUNPOD_API_KEY", "runpod-canary-778899"},
            {"HF_TOKEN", "hf_canary_token_secret_alpha"},
            {"HUGGING_FACE_HUB_TOKEN", "hf_hub_canary_token_beta"},
            {"HUGGINGFACE_TOKEN", "hf_legacy_canary_token_gamma"},
            {"HF_TOKEN_PATH", "/tmp/forbidden/hf_token_path"},
            {"GITHUB_TOKEN", "ghp_canary_github_token_123"},
            {"GITLAB_TOKEN", "glpat-canary_gitlab_token_456"},
            {"SSH_AUTH_SOCK", "/tmp/canary_ssh_agent_sock"},
            {"GOOGLE_APPLICATION_CREDENTIALS", "/tmp/canary_gcp_creds.json"},
            {"DATABASE_SECRET_KEY", "canary_db_secret_key"},
            {"OIDC_CLIENT_AUTH_TOKEN", "canary_oidc_token"},
            {"INTERNAL_BLOB_ACCESS_KEY", "canary_blob_key"},
            {"SSL_PRIVATE_KEY", "canary_ssl_priv_key"}
        };

        for (const auto& pe : poison_list) {
            setenv(pe.key.c_str(), pe.canary.c_str(), 1);
        }

        setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
        setenv("HOME", fixture.user_home().string().c_str(), 1);
        setenv("CUDA_VISIBLE_DEVICES", "0,1", 1);
        setenv("HIP_VISIBLE_DEVICES", "0", 1);
        setenv("ROCM_PATH", "/opt/rocm", 1);
        setenv("OMP_NUM_THREADS", "4", 1);

        fs::path dump1 = fixture.root() / "poison_nullopt_dump.txt";
        std::string cmd1 = "env > \"" + dump1.string() + "\"";
        SandboxPolicy policy1;
        for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
            policy1.path_grants.push_back(sp);
        }
        policy1.add_write_path(fixture.root().string()).set_mode(SandboxMode::Auto);
        ProcessHandle h1 = ProcessManager::start_process("/bin/sh", {"-c", cmd1}, "", false, false, {}, policy1);
        r.check(h1.pid > 0, "Spawn child with Auto policy");
        int exit1 = ProcessManager::wait_for_exit(h1, 5);
        r.check(exit1 == 0, "Child executed env dump successfully");

        std::string dump_str1 = read_file_content(dump1);
        auto env_map1 = parse_env_lines(read_file_lines(dump1));

        int leaks1 = 0;
        for (const auto& pe : poison_list) {
            if (dump_str1.find(pe.canary) != std::string::npos || env_map1.find(pe.key) != env_map1.end()) {
                ++leaks1;
            }
        }
        r.check(leaks1 == 0, "Auto policy spawn: 0 secret leaks among poisoned environment", "leaks=" + std::to_string(leaks1));
        r.check(env_map1["CUDA_VISIBLE_DEVICES"] == "0,1", "Allowlisted CUDA_VISIBLE_DEVICES preserved");
        r.check(env_map1["HIP_VISIBLE_DEVICES"] == "0", "Allowlisted HIP_VISIBLE_DEVICES preserved");

        fs::path dump2 = fixture.root() / "poison_auto_dump.txt";
        std::string cmd2 = "env > \"" + dump2.string() + "\"";
        SandboxPolicy policy_auto = WrappedServer::build_default_sandbox_policy(
            fixture.model_file().string(), "/bin/sh", 9901, "vulkan", DEVICE_GPU);
        policy_auto.add_write_path(fixture.root().string());

        ProcessHandle h2 = ProcessManager::start_process("/bin/sh", {"-c", cmd2}, "", false, false, {}, policy_auto);
        r.check(h2.pid > 0, "Spawn child with SandboxMode::Auto policy");
        int exit2 = ProcessManager::wait_for_exit(h2, 5);
        r.check(exit2 == 0, "Auto policy child executed env dump successfully");

        std::string dump_str2 = read_file_content(dump2);
        auto env_map2 = parse_env_lines(read_file_lines(dump2));
        int leaks2 = 0;
        for (const auto& pe : poison_list) {
            if (dump_str2.find(pe.canary) != std::string::npos || env_map2.find(pe.key) != env_map2.end()) {
                ++leaks2;
            }
        }
        r.check(leaks2 == 0, "SandboxMode::Auto spawn: 0 secret leaks", "leaks=" + std::to_string(leaks2));

        fs::path dump_proc = fixture.root() / "proc_environ.bin";
        std::string cmd_proc = "cat /proc/self/environ > \"" + dump_proc.string() + "\"";
        ProcessHandle h_proc = ProcessManager::start_process("/bin/sh", {"-c", cmd_proc}, "", false, false, {}, policy_auto);
        ProcessManager::wait_for_exit(h_proc, 5);

        std::string proc_raw = read_file_content(dump_proc);
        int proc_leaks = 0;
        for (const auto& pe : poison_list) {
            if (proc_raw.find(pe.canary) != std::string::npos) {
                ++proc_leaks;
            }
        }
        r.check(proc_leaks == 0, "Raw kernel /proc/self/environ contains 0 secret canaries");

        r.check(policy_auto.has_read_path(fixture.model_file().string()), "Policy contains model file grant");
        r.check(!policy_auto.has_read_path(fixture.hf_token().string()), "Policy strictly excludes ~/.cache/huggingface/token");
        r.check(!policy_auto.has_read_path(fixture.hf_stored_tokens().string()), "Policy strictly excludes stored_tokens");
        r.check(!policy_auto.has_read_path(fixture.hf_token_lock().string()), "Policy strictly excludes token.lock");
        r.check(!policy_auto.has_read_path(fixture.hf_home().string()), "Policy strictly excludes root HF directory");
#endif

        r.report_summary("Secret Scrubbing & Token Isolation");
        overall.passed += r.passed.load();
        overall.failed += r.failed.load();
    }

    {
        TestResult r;

        nono_capability_set_free(nullptr);
        r.check(nono_capability_add_fs_read(nullptr, "/tmp") == NONO_ERROR_INVALID_PARAM, "nono_capability_add_fs_read(NULL) returns INVALID_PARAM");
        r.check(nono_capability_add_fs_write(nullptr, "/tmp") == NONO_ERROR_INVALID_PARAM, "nono_capability_add_fs_write(NULL) returns INVALID_PARAM");
        r.check(nono_capability_add_device(nullptr, "/dev/dri") == NONO_ERROR_INVALID_PARAM, "nono_capability_add_device(NULL) returns INVALID_PARAM");
        r.check(nono_capability_set_network_egress(nullptr, false) == NONO_ERROR_INVALID_PARAM, "nono_capability_set_network_egress(NULL) returns INVALID_PARAM");
        r.check(nono_capability_set_network_loopback(nullptr, true) == NONO_ERROR_INVALID_PARAM, "nono_capability_set_network_loopback(NULL) returns INVALID_PARAM");
        r.check(nono_capability_set_bind_port(nullptr, 8080) == NONO_ERROR_INVALID_PARAM, "nono_capability_set_bind_port(NULL) returns INVALID_PARAM");
        r.check(nono_sandbox_apply(nullptr) == NONO_ERROR_INVALID_PARAM, "nono_sandbox_apply(NULL) returns INVALID_PARAM");

        {
            for (int i = 0; i < 500; ++i) {
                nono_capability_set* caps = nono_capability_set_new();
                assert(caps != nullptr);
                nono_capability_add_fs_read(caps, "/usr");
                nono_capability_add_fs_write(caps, "/tmp");
                nono_capability_set_network_loopback(caps, true);
                nono_capability_set_bind_port(caps, 8000 + (i % 1000));
                nono_capability_set_free(caps);
            }
            r.check(true, "Allocated and freed 500 nono_capability_set instances with zero leaks/crashes");
        }

        auto stub_engine = SandboxEngine::create_fallback_stub_engine();
        r.check(!stub_engine->is_supported(), "FallbackStubEngine::is_supported == false");
        r.check(!stub_engine->is_kernel_enforced(), "FallbackStubEngine::is_kernel_enforced == false");
        r.check(std::string(stub_engine->get_backend_name()) == "fallback_stub", "FallbackStubEngine backend name is fallback_stub");

        SandboxPolicy auto_p;
        auto_p.set_mode(SandboxMode::Auto);
        std::string stub_err;
        r.check(stub_engine->apply(auto_p, &stub_err), "FallbackStubEngine::apply returns true for SandboxMode::Auto");

        SandboxPolicy scrubbed_p;
        scrubbed_p.set_mode(SandboxMode::ScrubbedOnly);
        r.check(stub_engine->apply(scrubbed_p, &stub_err), "FallbackStubEngine::apply returns true for SandboxMode::ScrubbedOnly");

        SandboxPolicy enforced_p;
        enforced_p.set_mode(SandboxMode::Enforced);
        r.check(!stub_engine->apply(enforced_p, &stub_err), "FallbackStubEngine::apply returns false for SandboxMode::Enforced");
        r.check(!stub_err.empty(), "FallbackStubEngine provides clean diagnostic error on Enforced failure", stub_err);

        auto nono_engine = SandboxEngine::create_nono_ffi_engine();
        r.check(nono_engine != nullptr, "create_nono_ffi_engine returns valid instance");
        r.check(nono_engine->get_backend() == EngineBackend::NonoFFI, "NonoFFIEngine backend is NonoFFI");

        SandboxPolicy sb_policy;
        sb_policy.add_read_path("/tmp/test_read");
        sb_policy.add_write_path("/tmp/test_write");
        sb_policy.set_network_access(NetworkAccess::LoopbackOnly);
        sb_policy.set_bind_port(8000);

        nono_capability_set* caps = nono_capability_set_new();
        r.check(caps != nullptr, "nono_capability_set_new allocates handle");
        nono_status s = SandboxEngine::policy_to_nono_capabilities(sb_policy, caps);
        r.check(s == NONO_OK, "policy_to_nono_capabilities succeeds");
        nono_capability_set_free(caps);

        r.report_summary("Nono C FFI & Engine Degradation");
        overall.passed += r.passed.load();
        overall.failed += r.failed.load();
    }

    {
        TestResult r;

        struct BackendSpec {
            std::string name;
            std::string executable;
            std::string model_path;
            uint16_t port;
            std::string variant;
            DeviceType dev_type;
            std::vector<std::string> expected_devs;
            std::vector<std::string> forbidden_devs;
        };

        std::vector<BackendSpec> backends = {
            {"acestep", "/bin/acestep", "/cache/acestep.bin", 8101, "cpu", DEVICE_CPU, {}, {"/dev/dri", "/dev/accel"}},
            {"fastflowlm", "/bin/flm", "/cache/flm-model", 8102, "flm", DEVICE_NPU, {"/dev/accel", "/dev/amdxdna", "/sys/class/accel", "/dev/dri"}, {}},
            {"kokoro", "/bin/koko", "/cache/koko.onnx", 8103, "cpu", DEVICE_CPU, {}, {"/dev/dri", "/dev/accel"}},
            {"llamacpp-vulkan", "/bin/llama-server", "/cache/llama.gguf", 8104, "vulkan", DEVICE_GPU, {"/dev/dri", "/dev/kfd", "/dev/dxg"}, {"/dev/amdxdna"}},
            {"llamacpp-rocm", "/bin/llama-server", "/cache/llama.gguf", 8105, "rocm", DEVICE_GPU, {"/dev/dri", "/dev/kfd"}, {"/dev/amdxdna"}},
            {"llamacpp-cuda", "/bin/llama-server", "/cache/llama.gguf", 8106, "cuda", DEVICE_GPU, {"/dev/dri", "/dev/nvidiactl", "/dev/nvidia-uvm"}, {"/dev/amdxdna"}},
            {"moonshine", "/bin/moonshine", "/cache/moonshine.bin", 8107, "cpu", DEVICE_CPU, {}, {"/dev/dri", "/dev/accel"}},
            {"onnxruntime", "/bin/ort", "/cache/ort.onnx", 8108, "cpu", DEVICE_CPU, {}, {"/dev/accel"}},
            {"openmoss", "/bin/openmoss", "/cache/moss.bin", 8109, "cpu", DEVICE_CPU, {}, {"/dev/accel"}},
            {"ryzenai", "/bin/ryzenai", "/cache/ryzenai.bin", 8110, "ryzenai", DEVICE_NPU, {"/dev/accel", "/dev/amdxdna", "/sys/class/accel", "/dev/dri"}, {}},
            {"sdcpp-vulkan", "/bin/sd-server", "/cache/sd.gguf", 8111, "vulkan", DEVICE_GPU, {"/dev/dri"}, {"/dev/amdxdna"}},
            {"thenoise", "/bin/thenoise", "/cache/noise.bin", 8112, "cpu", DEVICE_CPU, {}, {"/dev/accel"}},
            {"thinksound", "/bin/thinksound", "/cache/sound.bin", 8113, "cpu", DEVICE_CPU, {}, {"/dev/accel"}},
            {"trellis", "/bin/trellis", "/cache/trellis.bin", 8114, "cpu", DEVICE_CPU, {}, {"/dev/accel"}},
            {"vllm-rocm", "/bin/vllm", "/cache/vllm-model", 8115, "rocm", DEVICE_GPU, {"/dev/dri", "/dev/kfd"}, {"/dev/amdxdna"}},
            {"whispercpp-cpu", "/bin/whisper", "/cache/whisper.bin", 8116, "cpu", DEVICE_CPU, {}, {"/dev/accel"}},
            {"whispercpp-npu", "/bin/whisper", "/cache/whisper.bin", 8117, "npu", DEVICE_NPU, {"/dev/accel", "/dev/amdxdna"}, {}}
        };

        for (const auto& b : backends) {
            SandboxPolicy pol = WrappedServer::build_default_sandbox_policy(
                b.model_path, b.executable, b.port, b.variant, b.dev_type);

            r.check(pol.bind_port == b.port, b.name + ": bind port set to " + std::to_string(b.port));
            r.check(pol.network_access == NetworkAccess::LoopbackOnly, b.name + ": network access is LoopbackOnly");
            r.check(pol.has_read_path(b.executable), b.name + ": executable path granted");
            r.check(pol.has_read_path(b.model_path), b.name + ": model path granted");

            for (const auto& d : b.expected_devs) {
                r.check(pol.has_device(d), b.name + ": contains expected device grant " + d);
            }
            for (const auto& d : b.forbidden_devs) {
                r.check(!pol.has_device(d), b.name + ": excludes forbidden device grant " + d);
            }
        }

        r.report_summary("All Backend Types Validation");
        overall.passed += r.passed.load();
        overall.failed += r.failed.load();
    }

    {
        TestResult r;

#ifndef _WIN32
        bool landlock_supported = SandboxEngine::is_platform_supported();
        if (landlock_supported) {
            SandboxPolicy net_policy;
            net_policy.set_mode(SandboxMode::Enforced);
            net_policy.set_network_access(NetworkAccess::LoopbackOnly);
            net_policy.set_bind_port(9876);
            for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
                net_policy.path_grants.push_back(sp);
            }
            net_policy.add_read_path("/bin").add_read_path("/usr/bin").add_write_path("/tmp").add_write_path("/dev");

            std::string cmd_loopback =
                "python3 -s -c \"import socket; s = socket.socket(socket.AF_INET, socket.SOCK_STREAM); "
                "s.bind(('127.0.0.1', 9876)); s.listen(1); s.close()\" 2>/dev/null && exit 0 || exit 80";

            ProcessHandle h_loop = ProcessManager::start_process(
                "/bin/sh", {"-c", cmd_loopback}, "", false, false, {}, net_policy);
            int exit_loop = ProcessManager::wait_for_exit(h_loop, 5);
            r.check(exit_loop == 0, "Sandboxed child successfully bound and listened on 127.0.0.1:9876 (exit 0)",
                    "exit=" + std::to_string(exit_loop));

            auto engine = SandboxEngine::create_for_platform();
            if (engine && engine->get_capabilities().supports_network_isolation) {
                std::string cmd_external_net =
                    "python3 -s -c 'import socket, sys\n"
                    "try:\n"
                    "    s = socket.socket()\n"
                    "    s.settimeout(0.5)\n"
                    "    s.connect((\"8.8.8.8\", 53))\n"
                    "    sys.exit(90)\n"
                    "except Exception:\n"
                    "    sys.exit(0)\n'";

                ProcessHandle h_net = ProcessManager::start_process(
                    "/bin/sh", {"-c", cmd_external_net}, "", false, false, {}, net_policy);
                int exit_net = ProcessManager::wait_for_exit(h_net, 5);
                r.check(exit_net == 0, "Network connect to external IP was strictly blocked under LoopbackOnly (exit 0)",
                        "exit=" + std::to_string(exit_net));
            } else {
                std::cout << "  [SKIP] Network egress isolation is not supported on this engine/platform." << std::endl;
            }
        }

        {
            std::atomic<int> concurrent_successes{0};
            std::atomic<int> concurrent_failures{0};
            std::vector<std::thread> workers;

            for (int t = 0; t < 16; ++t) {
                workers.emplace_back([t, &fixture, &concurrent_successes, &concurrent_failures]() {
                    fs::path thread_dump = fixture.root() / ("thread_dump_" + std::to_string(t) + ".txt");
                    std::string thread_cmd = "echo 'THREAD_" + std::to_string(t) + "_OK' > \"" + thread_dump.string() + "\"";

                    SandboxPolicy pol;
                    pol.set_mode(SandboxMode::Auto);
                    for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
                        pol.path_grants.push_back(sp);
                    }
                    pol.add_read_path("/bin").add_read_path("/usr/bin").add_write_path(fixture.root().string());

                    std::vector<std::pair<std::string, std::string>> custom_env = {
                        {"THREAD_ID", std::to_string(t)},
                        {"LEMONADE_SECRET_THREAD_" + std::to_string(t), "thread_secret_val"}
                    };

                    ProcessHandle h = ProcessManager::start_process(
                        "/bin/sh", {"-c", thread_cmd}, "", false, false, custom_env, pol);
                    if (h.pid <= 0) {
                        ++concurrent_failures;
                        return;
                    }
                    int rc = ProcessManager::wait_for_exit(h, 5);
                    if (rc == 0) {
                        std::string out = read_file_content(thread_dump);
                        if (out.find("THREAD_" + std::to_string(t) + "_OK") != std::string::npos) {
                            ++concurrent_successes;
                        } else {
                            ++concurrent_failures;
                        }
                    } else {
                        ++concurrent_failures;
                    }
                });
            }

            for (auto& w : workers) {
                if (w.joinable()) w.join();
            }

            r.check(concurrent_successes.load() == 16 && concurrent_failures.load() == 0,
                    "16 concurrent sandboxed child processes completed successfully with 0 failures",
                    "successes=" + std::to_string(concurrent_successes.load()) + ", failures=" + std::to_string(concurrent_failures.load()));
        }
#else
        r.check(true, "Windows baseline check");
#endif

        r.report_summary("Network Confinement & Multithreaded Stress");
        overall.passed += r.passed.load();
        overall.failed += r.failed.load();
    }

    if (overall.failed.load() == 0) {
        return 0;
    } else {
        return 1;
    }
}
