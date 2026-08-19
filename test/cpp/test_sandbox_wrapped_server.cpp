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
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
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
using lemon::sandbox::EnvScrubber;
using lemon::sandbox::NetworkAccess;
using lemon::sandbox::PathGrant;
using lemon::sandbox::PlatformDetector;
using lemon::sandbox::PlatformType;
using lemon::sandbox::PolicyPresets;
using lemon::sandbox::SandboxEngine;
using lemon::sandbox::SandboxMode;
using lemon::sandbox::SandboxPolicy;
using lemon::test::TempSandboxFixture;
using lemon::test::TestResult;
using lemon::utils::ProcessHandle;
using lemon::utils::ProcessManager;

class ConcreteWrappedServer : public WrappedServer {
public:
    ConcreteWrappedServer(const std::string& name, DeviceType dev = DEVICE_NONE)
        : WrappedServer(name, "debug") {
        device_type_ = dev;
    }
    void load(const std::string&, const ModelInfo&, const RecipeOptions&, bool) override {}
    void unload() override {}

    void set_test_device(DeviceType dev) {
        device_type_ = dev;
    }
};

int main() {
    TestResult total_results;
    TempSandboxFixture fixture("lemonade_test_hf_");

    {
        TestResult r;

#ifndef _WIN32
        setenv("HOME", fixture.user_home().string().c_str(), 1);
        unsetenv("HF_HOME");
        unsetenv("HF_HUB_CACHE");
        unsetenv("HF_TOKEN_PATH");
#else
        _putenv_s("USERPROFILE", fixture.user_home().string().c_str());
        _putenv_s("HOME", fixture.user_home().string().c_str());
        _putenv_s("HF_HOME", "");
        _putenv_s("HF_HUB_CACHE", "");
        _putenv_s("HF_TOKEN_PATH", "");
#endif

        std::string model_file_str = fixture.model_file().string();
        SandboxPolicy p1 = WrappedServer::build_default_sandbox_policy(
            model_file_str, "/usr/bin/llama-server", 8080, "vulkan", DEVICE_GPU);

        r.check(!p1.has_read_path(fixture.hf_token().string()), "policy does NOT contain HF token grant");
        r.check(!p1.has_read_path(fixture.hf_stored_tokens().string()), "policy does NOT contain stored_tokens grant");
        r.check(!p1.has_read_path(fixture.hf_token_lock().string()), "policy does NOT contain token.lock grant");
        r.check(!p1.has_read_path(fixture.hf_home().string()), "policy does NOT contain root HF home grant");

        r.check(p1.has_read_path(fixture.model_file().string()) ||
                p1.has_read_path(fixture.model_dir().string()),
                "policy contains snapshot model path grant");

        SandboxPolicy p2 = WrappedServer::build_default_sandbox_policy(
            fixture.model_dir().string(), "/usr/bin/llama-server", 8081, "cpu", DEVICE_CPU);
        r.check(!p2.has_read_path(fixture.hf_token().string()), "snapshot dir policy omits token file");

        SandboxPolicy p3 = WrappedServer::build_default_sandbox_policy(
            fixture.hf_hub().string(), "/usr/bin/llama-server", 8082, "rocm", DEVICE_GPU);
        r.check(!p3.has_read_path(fixture.hf_home().string()), "hub cache dir policy does NOT escalate to HF home root");
        r.check(!p3.has_read_path(fixture.hf_token().string()), "hub cache dir policy does NOT contain token file");

        SandboxPolicy p6 = WrappedServer::build_default_sandbox_policy(
            fixture.model_dir().string(), "/usr/bin/llama-server", 8085, "cpu", DEVICE_NONE);
        r.check(!p6.has_read_path(fixture.hf_token().string()), "direct token path never included in policy");

        ConcreteWrappedServer server("llama-server", DEVICE_GPU);
        SandboxPolicy p9 = server.build_sandbox_policy(
            "/usr/bin/llama-server", fixture.model_file().string(), 8088, "vulkan");
#if !defined(_WIN32) && !defined(__APPLE__)
        r.check(p9.has_device("/dev/dri"), "Polymorphic server inherits device grants");
#endif
        r.check(p9.bind_port == 8088, "Polymorphic server sets bind_port");

        r.report_summary("Model Path Scoping & Token Safety");
        total_results.passed += r.passed.load();
        total_results.failed += r.failed.load();
    }

    {
        TestResult r;

#ifndef _WIN32
        bool landlock_supported = SandboxEngine::is_platform_supported();
        if (landlock_supported) {
            SandboxPolicy policy;
            policy.set_mode(SandboxMode::Enforced);
            policy.set_network_access(NetworkAccess::LoopbackOnly);
            policy.set_bind_port(9500);

            for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
                policy.path_grants.push_back(sp);
            }
            policy.add_read_path("/bin")
                  .add_read_path("/usr/bin")
                  .add_write_path("/dev");

            policy.add_read_path(fixture.allowed_dir().string());
            policy.add_write_path(fixture.writeable_dir().string());

            std::string cmd_read_allowed =
                "cat \"" + fixture.allowed_file().string() + "\" >/dev/null 2>&1 && exit 0 || exit 10";
            ProcessHandle h1 = ProcessManager::start_process(
                "/bin/sh", {"-c", cmd_read_allowed}, "", false, false, {}, policy);
            r.check(h1.pid > 0, "start_process spawned child for granted read test");
            int exit1 = ProcessManager::wait_for_exit(h1, 5);
            r.check(exit1 == 0, "Child successfully read granted file inside allowed_dir (exit 0)",
                    "exit code: " + std::to_string(exit1));

            std::string cmd_write_allowed =
                "echo 'APPENDED_DATA' >> \"" + fixture.writeable_file().string() + "\" && exit 0 || exit 11";
            ProcessHandle h2 = ProcessManager::start_process(
                "/bin/sh", {"-c", cmd_write_allowed}, "", false, false, {}, policy);
            int exit2 = ProcessManager::wait_for_exit(h2, 5);
            r.check(exit2 == 0, "Child successfully wrote to granted writeable_dir (exit 0)",
                    "exit code: " + std::to_string(exit2));

            std::ifstream win(fixture.writeable_file());
            std::string wcontent((std::istreambuf_iterator<char>(win)), std::istreambuf_iterator<char>());
            r.check(wcontent.find("APPENDED_DATA") != std::string::npos,
                    "Write to writeable_dir persisted correctly");

            std::string cmd_write_ro =
                "echo 'UNAUTHORIZED_WRITE' > \"" + (fixture.allowed_dir() / "new_file.txt").string() + "\" 2>/dev/null && exit 20 || exit 0";
            ProcessHandle h3 = ProcessManager::start_process(
                "/bin/sh", {"-c", cmd_write_ro}, "", false, false, {}, policy);
            int exit3 = ProcessManager::wait_for_exit(h3, 5);
            r.check(exit3 == 0, "Child was strictly BLOCKED from writing to read-only directory (exit 0)",
                    "exit code: " + std::to_string(exit3));

            std::string cmd_read_forbidden =
                "cat \"" + fixture.forbidden_canary().string() + "\" >/dev/null 2>&1 && exit 30 || exit 0";
            ProcessHandle h4 = ProcessManager::start_process(
                "/bin/sh", {"-c", cmd_read_forbidden}, "", false, false, {}, policy);
            int exit4 = ProcessManager::wait_for_exit(h4, 5);
            r.check(exit4 == 0, "Child was strictly BLOCKED from reading unauthorized canary in forbidden_dir (exit 0)",
                    "exit code: " + std::to_string(exit4));

            std::string cmd_read_parent =
                "cat \"" + fixture.parent_canary().string() + "\" >/dev/null 2>&1 && exit 40 || exit 0";
            ProcessHandle h5 = ProcessManager::start_process(
                "/bin/sh", {"-c", cmd_read_parent}, "", false, false, {}, policy);
            int exit5 = ProcessManager::wait_for_exit(h5, 5);
            r.check(exit5 == 0, "Child was strictly BLOCKED from reading parent SSH key/canary (exit 0)",
                    "exit code: " + std::to_string(exit5));

            std::string cmd_read_shadow =
                "cat /etc/shadow >/dev/null 2>&1 && exit 50 || exit 0";
            ProcessHandle h6 = ProcessManager::start_process(
                "/bin/sh", {"-c", cmd_read_shadow}, "", false, false, {}, policy);
            int exit6 = ProcessManager::wait_for_exit(h6, 5);
            r.check(exit6 == 0, "Child was strictly BLOCKED from reading /etc/shadow (exit 0)",
                    "exit code: " + std::to_string(exit6));

            std::string cmd_ls_forbidden =
                "ls \"" + fixture.forbidden_dir().string() + "\" >/dev/null 2>&1 && exit 60 || exit 0";
            ProcessHandle h7 = ProcessManager::start_process(
                "/bin/sh", {"-c", cmd_ls_forbidden}, "", false, false, {}, policy);
            int exit7 = ProcessManager::wait_for_exit(h7, 5);
            r.check(exit7 == 0, "Child was strictly BLOCKED from directory listing ungranted directory (exit 0)",
                    "exit code: " + std::to_string(exit7));

            SandboxPolicy hf_live_policy = WrappedServer::build_default_sandbox_policy(
                fixture.model_file().string(), "/bin/sh", 9501, "cpu", DEVICE_NONE);
            hf_live_policy.set_mode(SandboxMode::Enforced);
            for (const auto& sp : PolicyPresets::get_standard_system_paths()) {
                hf_live_policy.path_grants.push_back(sp);
            }
            hf_live_policy.add_read_path("/bin").add_read_path("/usr/bin").add_write_path("/dev");
            hf_live_policy.normalize_paths();

            std::string cmd_hf_live =
                "cat \"" + fixture.model_file().string() + "\" >/dev/null 2>&1 || exit 70; "
                "if cat \"" + fixture.hf_token().string() + "\" >/dev/null 2>&1; then "
                "  exit 71; "
                "else "
                "  exit 0; "
                "fi";

            ProcessHandle h8 = ProcessManager::start_process(
                "/bin/sh", {"-c", cmd_hf_live}, "", false, false, {}, hf_live_policy);
            int exit8 = ProcessManager::wait_for_exit(h8, 5);
            r.check(exit8 == 0, "Child under WrappedServer policy can read model but is BLOCKED from reading HF token (exit 0)",
                    "exit code: " + std::to_string(exit8));

        } else {
            SandboxPolicy policy;
            policy.set_mode(SandboxMode::Auto);
            ProcessHandle h = ProcessManager::start_process(
                "/bin/sh", {"-c", "exit 0"}, "", false, false, {}, policy);
            r.check(h.pid > 0, "Auto mode spawns child on unsupported host");
            r.check(ProcessManager::wait_for_exit(h, 5) == 0, "Auto mode child exits 0");
        }
#else
        SandboxPolicy policy;
        policy.set_mode(SandboxMode::Auto);
        ProcessHandle h = ProcessManager::start_process(
            "cmd.exe", {"/c", "exit 0"}, "", false, false, {}, policy);
        r.check(h.pid > 0 || h.handle != nullptr, "Windows spawn with policy succeeds");
        r.check(ProcessManager::wait_for_exit(h, 5) == 0, "Windows child exits 0");
#endif

        r.report_summary("In-Child Filesystem Confinement");
        total_results.passed += r.passed.load();
        total_results.failed += r.failed.load();
    }

    {
        TestResult r;

        struct BackendMatrixCase {
            std::string label;
            std::string executable;
            std::string model_path;
            uint16_t port;
            std::string variant;
            DeviceType device_type;
            std::vector<std::string> required_devices;
            std::vector<std::string> forbidden_devices;
            NetworkAccess expected_net;
        };

        std::vector<BackendMatrixCase> test_cases = {
            {
                "Llama.cpp Vulkan (GPU)",
                "/usr/bin/llama-server",
                "/cache/models/llama-3.gguf",
                8001,
                "vulkan",
                DEVICE_GPU,
                {"/dev/dri", "/dev/kfd", "/dev/dxg"},
                {"/dev/amdxdna"},
                NetworkAccess::LoopbackOnly
            },
            {
                "Llama.cpp ROCm (GPU)",
                "/opt/lemonade/bin/llama-server",
                "/cache/models/qwen.gguf",
                8002,
                "rocm",
                DEVICE_GPU,
                {"/dev/dri", "/dev/kfd"},
                {"/dev/amdxdna"},
                NetworkAccess::LoopbackOnly
            },
            {
                "Llama.cpp CUDA (GPU)",
                "/usr/local/bin/llama-server",
                "/cache/models/deepseek.gguf",
                8003,
                "cuda",
                DEVICE_GPU,
                {"/dev/dri", "/dev/nvidiactl", "/dev/nvidia-uvm"},
                {"/dev/amdxdna"},
                NetworkAccess::LoopbackOnly
            },
            {
                "Llama.cpp CPU (No Accelerator)",
                "/usr/bin/llama-server",
                "/cache/models/phi.gguf",
                8004,
                "cpu",
                DEVICE_CPU,
                {},
                {"/dev/kfd", "/dev/accel", "/dev/amdxdna"},
                NetworkAccess::LoopbackOnly
            },
            {
                "FastFlowLM (NPU)",
                "/usr/bin/flm",
                "/cache/models/flm-model",
                8005,
                "flm",
                DEVICE_NPU,
                {"/dev/accel", "/dev/amdxdna", "/sys/class/accel", "/dev/dri"},
                {"/dev/nvidiactl"},
                NetworkAccess::LoopbackOnly
            },
            {
                "RyzenAI LLM (NPU)",
                "/opt/ryzenai/bin/ryzenai-server",
                "/cache/models/ryzenai-weights",
                8006,
                "ryzenai",
                DEVICE_NPU,
                {"/dev/accel", "/dev/amdxdna", "/sys/class/accel", "/dev/dri"},
                {},
                NetworkAccess::LoopbackOnly
            },
            {
                "vLLM ROCm (GPU / Strix Halo)",
                "/opt/vllm/bin/python",
                "/cache/models/vllm-llama",
                8007,
                "rocm",
                DEVICE_GPU,
                {"/dev/dri", "/dev/kfd"},
                {"/dev/amdxdna"},
                NetworkAccess::LoopbackOnly
            },
            {
                "Whisper.cpp CPU",
                "/usr/bin/whisper-server",
                "/cache/models/ggml-whisper-base.bin",
                8008,
                "cpu",
                DEVICE_CPU,
                {},
                {"/dev/kfd", "/dev/accel"},
                NetworkAccess::LoopbackOnly
            },
            {
                "Whisper.cpp NPU",
                "/usr/bin/whisper-server",
                "/cache/models/ggml-whisper-base.bin",
                8009,
                "npu",
                DEVICE_NPU,
                {"/dev/accel", "/dev/amdxdna"},
                {},
                NetworkAccess::LoopbackOnly
            },
            {
                "Stable Diffusion GPU (Vulkan)",
                "/usr/bin/sd-server",
                "/cache/models/sd-v1-5.gguf",
                8010,
                "vulkan",
                DEVICE_GPU,
                {"/dev/dri"},
                {"/dev/amdxdna"},
                NetworkAccess::LoopbackOnly
            },
            {
                "Kokoro TTS CPU",
                "/usr/bin/koko",
                "/cache/models/kokoro-v0_19.onnx",
                8011,
                "cpu",
                DEVICE_CPU,
                {},
                {"/dev/kfd", "/dev/accel"},
                NetworkAccess::LoopbackOnly
            },
            {
                "Moonshine ASR CPU",
                "/usr/bin/moonshine-server",
                "/cache/models/moonshine-base",
                8012,
                "cpu",
                DEVICE_CPU,
                {},
                {"/dev/kfd", "/dev/accel"},
                NetworkAccess::LoopbackOnly
            }
        };

        for (const auto& tc : test_cases) {
            SandboxPolicy policy = WrappedServer::build_default_sandbox_policy(
                tc.model_path, tc.executable, tc.port, tc.variant, tc.device_type);

            r.check(policy.bind_port == tc.port, tc.label + ": bind_port configured to " + std::to_string(tc.port));
            r.check(policy.network_access == tc.expected_net, tc.label + ": network_access matches LoopbackOnly");
            r.check(policy.has_read_path(tc.executable), tc.label + ": executable read grant present");
            r.check(policy.has_read_path(tc.model_path), tc.label + ": model_path read grant present");

#if !defined(_WIN32) && !defined(__APPLE__)
            for (const auto& dev : tc.required_devices) {
                r.check(policy.has_device(dev), tc.label + ": contains required device " + dev);
            }

            for (const auto& dev : tc.forbidden_devices) {
                r.check(!policy.has_device(dev), tc.label + ": correctly excludes device " + dev);
            }
#endif

            r.check(policy.has_allowed_env("PATH"), tc.label + ": allows PATH env");
#ifndef _WIN32
            r.check(policy.has_allowed_env("HOME"), tc.label + ": allows HOME env");
#else
            r.check(policy.has_allowed_env("USERPROFILE"), tc.label + ": allows USERPROFILE env");
#endif
            r.check(policy.has_allowed_env("CUDA_VISIBLE_DEVICES"), tc.label + ": allows CUDA_VISIBLE_DEVICES env");
            r.check(policy.has_allowed_env("HIP_VISIBLE_DEVICES"), tc.label + ": allows HIP_VISIBLE_DEVICES env");
        }

#if !defined(_WIN32) && !defined(__APPLE__)
        std::vector<std::string> vulkan_casings = {"vulkan", "VULKAN", "Vulkan", "vUlKaN"};
        for (const auto& v : vulkan_casings) {
            SandboxPolicy pol = WrappedServer::build_default_sandbox_policy(
                "/tmp/m.gguf", "/bin/exec", 8090, v, DEVICE_NONE);
            r.check(pol.has_device("/dev/dri"), "Case insensitivity check for variant '" + v + "' grants /dev/dri");
        }

        std::vector<std::string> rocm_casings = {"rocm", "ROCM", "Rocm", "RoCm"};
        for (const auto& v : rocm_casings) {
            SandboxPolicy pol = WrappedServer::build_default_sandbox_policy(
                "/tmp/m.gguf", "/bin/exec", 8091, v, DEVICE_NONE);
            r.check(pol.has_device("/dev/kfd"), "Case insensitivity check for variant '" + v + "' grants /dev/kfd");
        }

        std::vector<std::string> npu_casings = {"npu", "NPU", "Npu"};
        for (const auto& v : npu_casings) {
            SandboxPolicy pol = WrappedServer::build_default_sandbox_policy(
                "/tmp/m.gguf", "/bin/exec", 8092, v, DEVICE_NONE);
            r.check(pol.has_device("/dev/accel"), "Case insensitivity check for variant '" + v + "' grants /dev/accel");
        }

        SandboxPolicy p_llama = WrappedServer::build_default_sandbox_policy("/cache/model.gguf", "/usr/bin/llama-server", 8000, "vulkan", DEVICE_GPU);
        r.check(p_llama.has_device("/dev/dri"), "WrappedServer llamacpp policy includes /dev/dri");
        r.check(p_llama.bind_port == 8000, "WrappedServer llamacpp policy sets port 8000");

        SandboxPolicy p_flm = WrappedServer::build_default_sandbox_policy("/cache/flm-model", "/usr/bin/flm", 8001, "npu", DEVICE_NPU);
        r.check(p_flm.has_device("/dev/accel"), "WrappedServer fastflowlm policy includes /dev/accel");
        r.check(p_flm.has_device("/dev/amdxdna"), "WrappedServer fastflowlm policy includes /dev/amdxdna");

        SandboxPolicy p_vllm = WrappedServer::build_default_sandbox_policy("/cache/vllm-model", "/usr/bin/vllm", 8002, "rocm", DEVICE_GPU);
        r.check(p_vllm.has_device("/dev/kfd"), "WrappedServer vllm policy includes /dev/kfd");
        r.check(p_vllm.has_read_path("/opt/rocm"), "WrappedServer vllm policy includes /opt/rocm");

        SandboxPolicy p_whisper = WrappedServer::build_default_sandbox_policy("/cache/whisper.bin", "/usr/bin/whisper-server", 8003, "npu", DEVICE_NPU);
        r.check(p_whisper.has_device("/dev/accel"), "WrappedServer whispercpp (npu) policy includes /dev/accel");

        SandboxPolicy p_sd = WrappedServer::build_default_sandbox_policy("/cache/sd.gguf", "/usr/bin/sd-server", 8004, "vulkan", DEVICE_GPU);
        r.check(p_sd.has_device("/dev/dri"), "WrappedServer sdcpp policy includes /dev/dri");
#endif

        // RecipeOptions sandbox overrides
        nlohmann::json recipe_json = {
            {"sandbox", {
                {"mode", "enforced"},
                {"network_access", "full"},
                {"path_grants", {{{"path", "/tmp/recipe_custom_dir"}, {"write_allowed", true}}}},
                {"allowed_env_vars", {"RECIPE_CUSTOM_VAR"}}
            }}
        };
        RecipeOptions recipe_opts("custom_recipe", recipe_json);
        SandboxPolicy p_override = WrappedServer::build_default_sandbox_policy(
            "/cache/model.gguf", "/usr/bin/llama-server", 8005, "vulkan", DEVICE_GPU, &recipe_opts);
        r.check(p_override.mode == SandboxMode::Enforced, "RecipeOptions override can upgrade mode from Auto to Enforced");
        r.check(p_override.network_access == NetworkAccess::Full, "RecipeOptions override updates network_access to full");
        r.check(p_override.has_write_path("/tmp/recipe_custom_dir"), "RecipeOptions override adds custom write path");
        r.check(p_override.has_allowed_env("RECIPE_CUSTOM_VAR"), "RecipeOptions override adds custom allowed env");

        // RecipeOptions cannot downgrade Auto to Disabled
        nlohmann::json recipe_downgrade_json = {
            {"sandbox", {
                {"mode", "disabled"}
            }}
        };
        RecipeOptions recipe_downgrade_opts("custom_recipe", recipe_downgrade_json);
        SandboxPolicy p_no_downgrade = WrappedServer::build_default_sandbox_policy(
            "/cache/model.gguf", "/usr/bin/llama-server", 8005, "vulkan", DEVICE_GPU, &recipe_downgrade_opts);
        r.check(p_no_downgrade.mode == SandboxMode::Auto, "RecipeOptions cannot downgrade Auto mode to Disabled");

        // hf_load recipe policy specialization
        RecipeOptions hf_load_opts("hf_load", nlohmann::json::object());
        SandboxPolicy p_hf = WrappedServer::build_default_sandbox_policy(
            "", "/usr/bin/llama-server", 8006, "hf_load", DEVICE_GPU, &hf_load_opts);
        r.check(p_hf.network_access == NetworkAccess::Full, "hf_load recipe grants full network access");
        const char* hf_user_home = std::getenv("HOME");
#ifdef _WIN32
        if (!hf_user_home) hf_user_home = std::getenv("USERPROFILE");
#endif
        if (hf_user_home) {
            std::filesystem::path llama_cache = std::filesystem::path(hf_user_home) / ".cache" / "llama.cpp";
            r.check(p_hf.has_write_path(llama_cache.string()), "hf_load recipe grants write access to ~/.cache/llama.cpp");
        }

        r.report_summary("Backend Policy Variation & Hardware Device Matrix");
        total_results.passed += r.passed.load();
        total_results.failed += r.failed.load();
    }

    if (total_results.failed.load() == 0) {
        return 0;
    } else {
        return 1;
    }
}
