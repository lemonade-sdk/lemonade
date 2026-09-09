#include <cstdio>
#include <string>
#include <vector>

#include "lemon/sandbox/sandbox_policy.h"
#include "sandbox_test_utils.h"

using lemon::sandbox::NetworkAccess;
using lemon::sandbox::PathGrant;
using lemon::sandbox::PolicyPresets;
using lemon::sandbox::SandboxMode;
using lemon::sandbox::SandboxPolicy;
using lemon::test::TestResult;

int main() {
    TestResult r;

    {
        SandboxPolicy policy;
        r.check(policy.network_access == NetworkAccess::LoopbackOnly,
                "default policy sets network_access to LoopbackOnly");
        r.check(policy.mode == SandboxMode::Auto,
                "default policy sets mode to Auto");
        r.check(policy.bind_port == 0,
                "default policy sets bind_port to 0");
        r.check(policy.path_grants.empty(),
                "default policy has empty path_grants");
        r.check(policy.device_grants.empty(),
                "default policy has empty device_grants");
        r.check(policy.allowed_env_vars.empty(),
                "default policy has empty allowed_env_vars");
        r.check(policy.explicit_env_vars.empty(),
                "default policy has empty explicit_env_vars");
    }

    {
        SandboxPolicy policy;
        policy.add_read_path("/usr/lib")
              .add_write_path("/tmp/lemonade_runtime");

        r.check(policy.path_grants.size() == 2, "path_grants tracks added grants");
        r.check(policy.has_read_path("/usr/lib"), "has_read_path finds read path");
        r.check(!policy.has_write_path("/usr/lib"), "has_write_path returns false for read-only grant");
        r.check(policy.has_write_path("/tmp/lemonade_runtime"), "has_write_path finds write path");
        r.check(policy.has_read_path("/tmp/lemonade_runtime"), "has_read_path finds write-allowed path");
    }

    {
        SandboxPolicy policy;
        policy.add_device("/dev/dri/renderD128")
              .add_device("/dev/kfd")
              .add_device("/dev/dxg")
              .add_device("/dev/accel/accel0")
              .add_device("/dev/null")
              .add_device("/dev/urandom");

        r.check(policy.device_grants.size() == 6, "device_grants tracks all GPU/NPU/system devices");
        r.check(policy.has_device("/dev/dri/renderD128"), "DRI render device recorded");
        r.check(policy.has_device("/dev/kfd"), "ROCm KFD device recorded");
        r.check(policy.has_device("/dev/dxg"), "WSL2 DirectX device recorded");
        r.check(policy.has_device("/dev/accel/accel0"), "XDNA NPU device recorded");
        r.check(!policy.has_device("/dev/sda"), "unadded device returns false");
    }

    {
        SandboxPolicy policy;
        policy.set_network_access(NetworkAccess::DenyAll);
        r.check(policy.network_access == NetworkAccess::DenyAll, "network_access DenyAll supported");
        r.check(std::string(lemon::sandbox::network_access_to_string(policy.network_access)) == "deny_all",
                "network_access_to_string matches deny_all");

        policy.set_network_access(NetworkAccess::LoopbackOnly);
        policy.set_bind_port(8001);
        r.check(policy.network_access == NetworkAccess::LoopbackOnly, "network_access LoopbackOnly supported");
        r.check(policy.bind_port == 8001, "bind_port configured correctly");
        r.check(std::string(lemon::sandbox::network_access_to_string(policy.network_access)) == "loopback_only",
                "network_access_to_string matches loopback_only");

        policy.set_network_access(NetworkAccess::Full);
        r.check(policy.network_access == NetworkAccess::Full, "network_access Full supported");
        r.check(std::string(lemon::sandbox::network_access_to_string(policy.network_access)) == "full",
                "network_access_to_string matches full");
    }

    {
        SandboxPolicy policy;
        policy.set_mode(SandboxMode::Enforced);
        r.check(policy.mode == SandboxMode::Enforced, "SandboxMode::Enforced supported");
        r.check(std::string(lemon::sandbox::sandbox_mode_to_string(policy.mode)) == "enforced",
                "sandbox_mode_to_string matches enforced");

        policy.set_mode(SandboxMode::ScrubbedOnly);
        r.check(policy.mode == SandboxMode::ScrubbedOnly, "SandboxMode::ScrubbedOnly supported");
        r.check(std::string(lemon::sandbox::sandbox_mode_to_string(policy.mode)) == "scrubbed_only",
                "sandbox_mode_to_string matches scrubbed_only");

        policy.set_mode(SandboxMode::Auto);
        r.check(policy.mode == SandboxMode::Auto, "SandboxMode::Auto supported");
        r.check(std::string(lemon::sandbox::sandbox_mode_to_string(policy.mode)) == "auto",
                "sandbox_mode_to_string matches auto");

        policy.set_mode(SandboxMode::Disabled);
        r.check(policy.mode == SandboxMode::Disabled, "SandboxMode::Disabled supported");
        r.check(std::string(lemon::sandbox::sandbox_mode_to_string(policy.mode)) == "disabled",
                "sandbox_mode_to_string matches disabled");

        policy.set_mode(SandboxMode::Learn);
        r.check(policy.mode == SandboxMode::Learn, "SandboxMode::Learn supported");
        r.check(std::string(lemon::sandbox::sandbox_mode_to_string(policy.mode)) == "learn",
                "sandbox_mode_to_string matches learn");

        r.check(lemon::sandbox::parse_sandbox_mode("auto") == SandboxMode::Auto, "parse_sandbox_mode('auto')");
        r.check(lemon::sandbox::parse_sandbox_mode("enforced") == SandboxMode::Enforced, "parse_sandbox_mode('enforced')");
        r.check(lemon::sandbox::parse_sandbox_mode("disabled") == SandboxMode::Disabled, "parse_sandbox_mode('disabled')");
        r.check(lemon::sandbox::parse_sandbox_mode("off") == SandboxMode::Disabled, "parse_sandbox_mode('off')");
        r.check(lemon::sandbox::parse_sandbox_mode("0") == SandboxMode::Disabled, "parse_sandbox_mode('0')");
        r.check(lemon::sandbox::parse_sandbox_mode("scrubbed_only") == SandboxMode::ScrubbedOnly, "parse_sandbox_mode('scrubbed_only')");
        r.check(lemon::sandbox::parse_sandbox_mode("learn") == SandboxMode::Learn, "parse_sandbox_mode('learn')");
        r.check(lemon::sandbox::parse_sandbox_mode("profile") == SandboxMode::Learn, "parse_sandbox_mode('profile')");
    }

    {
        SandboxPolicy policy;
        policy.add_read_path("/usr/lib")
              .add_write_path("/tmp/test")
              .add_device("/dev/dri")
              .allow_env_var("PATH");
        policy.set_bind_port(8002);

        std::string dbg = policy.to_debug_string();
        r.check(!dbg.empty(), "to_debug_string is non-empty");
        r.check(dbg.find("mode=auto") != std::string::npos, "to_debug_string contains mode");
        r.check(dbg.find("bind_port=8002") != std::string::npos, "to_debug_string contains bind_port");
        r.check(dbg.find("/dev/dri") != std::string::npos, "to_debug_string contains device");

        std::string detail = policy.to_detailed_string();
        r.check(!detail.empty(), "to_detailed_string is non-empty");
        r.check(detail.find("[RO] /usr/lib") != std::string::npos, "to_detailed_string contains [RO] path");
        r.check(detail.find("[RW] /tmp/test") != std::string::npos, "to_detailed_string contains [RW] path");
        r.check(detail.find("/dev/dri") != std::string::npos, "to_detailed_string contains device");
    }

    {
        SandboxPolicy policy;
        policy.allow_env_vars({"PATH", "LD_LIBRARY_PATH", "ROCM_PATH"});
        policy.set_env_var("CUDA_VISIBLE_DEVICES", "0")
              .set_env_var("PYTHONNOUSERSITE", "1");

        r.check(policy.allowed_env_vars.size() == 3, "allowed_env_vars populated");
        r.check(policy.has_allowed_env("PATH"), "has_allowed_env finds PATH");
        r.check(!policy.has_allowed_env("SECRET_TOKEN"), "has_allowed_env returns false for unknown");
        r.check(policy.explicit_env_vars.size() == 2, "explicit_env_vars populated");
        r.check(policy.explicit_env_vars[0].first == "CUDA_VISIBLE_DEVICES" && policy.explicit_env_vars[0].second == "0",
                "explicit environment key-value preserved");
    }

    {
        SandboxPolicy policy;
        policy.add_read_path("/usr/lib")
              .add_write_path("/usr/lib")
              .add_read_path("/usr/lib/../lib")
              .add_read_path("/tmp/run/.")
              .add_read_path("");

        policy.normalize_paths();
        r.check(policy.path_grants.size() == 2, "normalize_paths deduplicates overlapping paths");
        r.check(policy.has_write_path("/usr/lib"), "write permission takes precedence on duplicate");
    }

    {
        auto sys_paths = PolicyPresets::get_standard_system_paths();
        r.check(!sys_paths.empty(), "get_standard_system_paths is non-empty");

        auto gpu_devs = PolicyPresets::get_standard_gpu_devices();
#ifndef __APPLE__
        r.check(!gpu_devs.empty(), "get_standard_gpu_devices is non-empty on Linux/Windows");
#endif

        auto npu_devs = PolicyPresets::get_standard_npu_devices();
#if !defined(_WIN32) && !defined(__APPLE__)
        r.check(!npu_devs.empty(), "get_standard_npu_devices is non-empty on Linux");
#endif

        auto env_vars = PolicyPresets::get_standard_allowed_env_vars();
        r.check(!env_vars.empty(), "get_standard_allowed_env_vars is non-empty");
    }

    {
        // 4-step pipeline testing
        SandboxPolicy p_step;
        r.check(p_step.path_grants.empty() && p_step.device_grants.empty(), "step 0: deny all baseline");

        // Step 1: System Runtime
        PolicyPresets::apply_system_runtime(p_step);
        r.check(!p_step.path_grants.empty(), "step 1: system runtime grants standard OS paths");
        r.check(p_step.has_allowed_env("PATH"), "step 1: system runtime allows PATH");

        // Step 2: Hardware profile (GPU)
        PolicyPresets::apply_hardware_profile(p_step, lemon::DEVICE_GPU, "vulkan");
#if !defined(__APPLE__) && !defined(_WIN32)
        r.check(p_step.has_device("/dev/dri"), "step 2: GPU hardware profile grants /dev/dri on Linux");
        r.check(p_step.has_read_path("/etc/vulkan"), "step 2: Vulkan hardware profile grants /etc/vulkan on Linux");
#endif

        // Step 3: Backend Workload Assets
        PolicyPresets::apply_backend_workload(p_step, "/usr/bin/llama-server", "/cache/models/llama.gguf");
        r.check(p_step.has_read_path("/usr/bin/llama-server"), "step 3: grants executable read");
        r.check(p_step.has_read_path("/cache/models/llama.gguf"), "step 3: grants model checkpoint read");

        // Step 4: Network Egress
        PolicyPresets::apply_network_egress(p_step);
        r.check(p_step.network_access == NetworkAccess::Full, "step 4: egress grants Full network");
        r.check(p_step.has_allowed_env("HTTP_PROXY"), "step 4: egress allows HTTP_PROXY");
    }

    {
        // JSON round-trip testing
        SandboxPolicy p1;
        p1.set_mode(SandboxMode::Enforced)
          .set_network_access(NetworkAccess::LoopbackOnly)
          .set_bind_port(8080)
          .add_read_path("/usr/lib")
          .add_write_path("/tmp/test")
          .add_device("/dev/dri")
          .allow_env_var("PATH")
          .set_env_var("CUDA_VISIBLE_DEVICES", "0");

        nlohmann::json j = p1;
        r.check(j["mode"] == "enforced", "json mode serialized correctly");
        r.check(j["network_access"] == "loopback_only", "json network_access serialized correctly");
        r.check(j["bind_port"] == 8080, "json bind_port serialized correctly");

        SandboxPolicy p2 = j.get<SandboxPolicy>();
        r.check(p2.mode == SandboxMode::Enforced, "from_json preserves mode");
        r.check(p2.network_access == NetworkAccess::LoopbackOnly, "from_json preserves network_access");
        r.check(p2.bind_port == 8080, "from_json preserves bind_port");
        r.check(p2.has_read_path("/usr/lib"), "from_json preserves read_path");
        r.check(p2.has_write_path("/tmp/test"), "from_json preserves write_path");
        r.check(p2.has_device("/dev/dri"), "from_json preserves device_grant");
        r.check(p2.has_allowed_env("PATH"), "from_json preserves allowed_env");
        r.check(p2.explicit_env_vars.size() == 1 && p2.explicit_env_vars[0].first == "CUDA_VISIBLE_DEVICES",
                "from_json preserves explicit_env");
    }

    {
        // JSON validation & error handling
        nlohmann::json bad_mode = {{"mode", "invalid_mode_typo"}};
        bool threw_mode = false;
        try {
            bad_mode.get<SandboxPolicy>();
        } catch (const std::invalid_argument&) {
            threw_mode = true;
        }
        r.check(threw_mode, "from_json throws on unknown sandbox mode");

        nlohmann::json bad_net = {{"network_access", "bogus_net"}};
        bool threw_net = false;
        try {
            bad_net.get<SandboxPolicy>();
        } catch (const std::invalid_argument&) {
            threw_net = true;
        }
        r.check(threw_net, "from_json throws on unknown network access");

        nlohmann::json bad_port = {{"bind_port", 70000}};
        bool threw_port = false;
        try {
            bad_port.get<SandboxPolicy>();
        } catch (const std::invalid_argument&) {
            threw_port = true;
        }
        r.check(threw_port, "from_json throws on out-of-range port");
    }

    {
        // validate_policy governance checks
        SandboxPolicy clean_policy;
        clean_policy.add_read_path("/usr/lib")
                    .add_write_path("/tmp/run")
                    .allow_env_var("PATH");
        std::string err;
        r.check(lemon::sandbox::validate_policy(clean_policy, &err), "validate_policy passes on clean policy", err);

        SandboxPolicy custom_env_policy;
        custom_env_policy.allow_env_var("HF_TOKEN");
        custom_env_policy.allow_env_var("MODELSCOPE_API_TOKEN");
        custom_env_policy.allow_env_var("OPENAI_API_KEY");
        custom_env_policy.allow_env_var("CUSTOM_REGISTRY_TOKEN");
        r.check(lemon::sandbox::validate_policy(custom_env_policy, &err),
                "validate_policy permits explicitly allowlisted backend/model environment variables", err);

        SandboxPolicy lemonade_key_policy;
        lemonade_key_policy.allow_env_var("LEMONADE_API_KEY");
        r.check(!lemon::sandbox::validate_policy(lemonade_key_policy, &err),
                "validate_policy rejects LEMONADE_API_KEY in allowed_env_vars");

        SandboxPolicy lemonade_admin_policy;
        lemonade_admin_policy.allow_env_var("LEMONADE_ADMIN_API_KEY");
        r.check(!lemon::sandbox::validate_policy(lemonade_admin_policy, &err),
                "validate_policy rejects LEMONADE_ADMIN_API_KEY in allowed_env_vars");

        SandboxPolicy root_slash_policy;
        root_slash_policy.add_read_path("/");
        r.check(!lemon::sandbox::validate_policy(root_slash_policy, &err),
                "validate_policy rejects root directory '/' in path_grants");

        SandboxPolicy etc_root_policy;
        etc_root_policy.add_read_path("/etc");
        r.check(!lemon::sandbox::validate_policy(etc_root_policy, &err),
                "validate_policy rejects system directory '/etc' in path_grants");

        SandboxPolicy home_root_policy;
        home_root_policy.add_read_path("/home");
        r.check(!lemon::sandbox::validate_policy(home_root_policy, &err),
                "validate_policy rejects broad '/home' root in path_grants");

        SandboxPolicy traversal_policy;
        traversal_policy.add_read_path("/tmp/models/../../etc/shadow");
        r.check(!lemon::sandbox::validate_policy(traversal_policy, &err),
                "validate_policy rejects directory traversal sequence '..' in path_grants");

        SandboxPolicy dev_gpu_policy;
        dev_gpu_policy.add_device("/dev/dri/renderD128").add_device("/dev/kfd").add_device("/dev/accel/accel0");
        r.check(lemon::sandbox::validate_policy(dev_gpu_policy, &err),
                "validate_policy permits standard accelerator devices", err);

        SandboxPolicy lemonade_hip_policy;
        lemonade_hip_policy.allow_env_var("LEMONADE_GGML_HIP_PATH");
        r.check(lemon::sandbox::validate_policy(lemonade_hip_policy, &err),
                "validate_policy allows non-secret operational path variable LEMONADE_GGML_HIP_PATH");
    }

    {
        // SandboxPolicy::merge() tests
        SandboxPolicy base;
        base.set_mode(SandboxMode::Auto)
            .set_network_access(NetworkAccess::LoopbackOnly)
            .add_read_path("/usr/lib")
            .allow_env_var("PATH");

        SandboxPolicy override_frag;
        override_frag.set_network_access(NetworkAccess::Full)
                     .add_write_path("/tmp/custom_cache")
                     .allow_env_var("CUSTOM_FLAG");

        base.merge(override_frag);
        r.check(base.network_access == NetworkAccess::Full, "merge updates network_access");
        r.check(base.has_read_path("/usr/lib"), "merge preserves base read path");
        r.check(base.has_write_path("/tmp/custom_cache"), "merge adds override write path");
        r.check(base.has_allowed_env("PATH"), "merge preserves base allowed env");
        r.check(base.has_allowed_env("CUSTOM_FLAG"), "merge adds override allowed env");
    }

    r.report_summary("SandboxPolicy");
    return r.exit_code();
}
