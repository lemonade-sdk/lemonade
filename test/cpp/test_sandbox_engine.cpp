#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lemon/sandbox/nono_ffi.h"
#include "lemon/sandbox/sandbox_engine.h"
#include "lemon/sandbox/sandbox_policy.h"
#include "sandbox_test_utils.h"

using lemon::sandbox::EngineBackend;
using lemon::sandbox::EngineCapabilities;
using lemon::sandbox::NetworkAccess;
using lemon::sandbox::PlatformDetector;
using lemon::sandbox::PlatformType;
using lemon::sandbox::SandboxEngine;
using lemon::sandbox::SandboxMode;
using lemon::sandbox::SandboxPolicy;
using lemon::test::TestResult;

int main() {
    TestResult r;

    {
        auto p1 = PlatformDetector::parse_platform("6.5.0-35-generic", "Linux version 6.5.0-35-generic (buildd@lcy02-amd64-070)", false, false, false);
        r.check(p1 == PlatformType::LinuxNative, "parse_platform identifies standard Linux");

        auto p2 = PlatformDetector::parse_platform("5.15.153.1-microsoft-standard-WSL2", "Linux version 5.15.153.1", false, false, false);
        r.check(p2 == PlatformType::LinuxWSL2, "parse_platform identifies WSL2 from osrelease");

        auto p3 = PlatformDetector::parse_platform("5.15.0", "Linux version 5.15.90.1-microsoft-standard-WSL2 (oe-user@oe-host)", false, false, false);
        r.check(p3 == PlatformType::LinuxWSL2, "parse_platform identifies WSL2 from proc_version");

        auto p4 = PlatformDetector::parse_platform("5.15.0", "Linux version 5.15.0-generic", true, false, false);
        r.check(p4 == PlatformType::LinuxWSL2, "parse_platform identifies WSL2 from /dev/dxg presence");

        auto p5 = PlatformDetector::parse_platform("", "", false, false, true);
        r.check(p5 == PlatformType::WindowsNative, "parse_platform identifies Windows Native");

        auto p6 = PlatformDetector::parse_platform("", "", false, true, false);
        r.check(p6 == PlatformType::MacOS, "parse_platform identifies macOS");

        r.check(std::string(lemon::sandbox::platform_type_to_string(PlatformType::LinuxNative)) == "linux_native",
                "platform_type_to_string linux_native");
        r.check(std::string(lemon::sandbox::platform_type_to_string(PlatformType::LinuxWSL2)) == "linux_wsl2",
                "platform_type_to_string linux_wsl2");
        r.check(std::string(lemon::sandbox::platform_type_to_string(PlatformType::MacOS)) == "macos",
                "platform_type_to_string macos");
        r.check(std::string(lemon::sandbox::platform_type_to_string(PlatformType::WindowsNative)) == "windows_native",
                "platform_type_to_string windows_native");
        r.check(std::string(lemon::sandbox::platform_type_to_string(PlatformType::Unknown)) == "unknown",
                "platform_type_to_string unknown");
    }

    {
        auto stub = SandboxEngine::create_fallback_stub_engine();
        r.check(stub != nullptr, "create_fallback_stub_engine returns valid instance");
        r.check(stub->get_backend() == EngineBackend::FallbackStub, "stub backend is FallbackStub");
        r.check(std::string(stub->get_backend_name()) == "fallback_stub", "stub backend name is fallback_stub");
        r.check(stub->is_supported() == false, "stub engine reports unsupported");
        r.check(stub->is_kernel_enforced() == false, "stub engine is not kernel enforced");

        SandboxPolicy policy;
        policy.set_mode(SandboxMode::Auto);
        r.check(stub->apply(policy) == true, "stub apply succeeds in Auto mode");

        policy.set_mode(SandboxMode::ScrubbedOnly);
        r.check(stub->apply(policy) == true, "stub apply succeeds in ScrubbedOnly mode");

        policy.set_mode(SandboxMode::Enforced);
        std::string err;
        r.check(stub->apply(policy, &err) == false, "stub apply returns false in Enforced mode");
        r.check(!err.empty(), "stub apply provides error message in Enforced mode");
    }

    {
        nono_capability_set* caps = nono_capability_set_new();
        r.check(caps != nullptr, "nono_capability_set_new allocates handle");

        r.check(nono_capability_add_fs_read(caps, "/models/llama.gguf") == NONO_OK,
                "nono_capability_add_fs_read succeeds");
        r.check(nono_capability_add_fs_write(caps, "/tmp/out") == NONO_OK,
                "nono_capability_add_fs_write succeeds");

        r.check(nono_capability_add_device(caps, "/dev/dri/renderD128") == NONO_OK,
                "nono_capability_add_device succeeds");

        r.check(nono_capability_set_network_egress(caps, false) == NONO_OK,
                "nono_capability_set_network_egress succeeds");
        r.check(nono_capability_set_network_loopback(caps, true) == NONO_OK,
                "nono_capability_set_network_loopback succeeds");
        r.check(nono_capability_set_bind_port(caps, 8080) == NONO_OK,
                "nono_capability_set_bind_port succeeds");

        r.check(nono_capability_add_fs_read(nullptr, "/path") == NONO_ERROR_INVALID_PARAM,
                "nono_capability_add_fs_read rejects NULL caps");
        r.check(nono_capability_add_fs_read(caps, nullptr) == NONO_ERROR_INVALID_PARAM,
                "nono_capability_add_fs_read rejects NULL path");
        r.check(nono_capability_add_fs_read(caps, "") == NONO_ERROR_INVALID_PARAM,
                "nono_capability_add_fs_read rejects empty path");
        r.check(nono_capability_add_fs_write(nullptr, "/path") == NONO_ERROR_INVALID_PARAM,
                "nono_capability_add_fs_write rejects NULL caps");
        r.check(nono_capability_add_device(nullptr, "/dev/null") == NONO_ERROR_INVALID_PARAM,
                "nono_capability_add_device rejects NULL caps");
        r.check(nono_capability_set_network_egress(nullptr, true) == NONO_ERROR_INVALID_PARAM,
                "nono_capability_set_network_egress rejects NULL caps");
        r.check(nono_capability_set_network_loopback(nullptr, true) == NONO_ERROR_INVALID_PARAM,
                "nono_capability_set_network_loopback rejects NULL caps");
        r.check(nono_capability_set_bind_port(nullptr, 8080) == NONO_ERROR_INVALID_PARAM,
                "nono_capability_set_bind_port rejects NULL caps");
        r.check(nono_sandbox_apply(nullptr) == NONO_ERROR_INVALID_PARAM,
                "nono_sandbox_apply rejects NULL caps");

        const char* err_str = nono_get_last_error();
        r.check(err_str != nullptr && std::string(err_str).find("Invalid") != std::string::npos,
                "nono_get_last_error returns last error");

        r.check(std::string(nono_status_to_string(NONO_OK)) == "NONO_OK", "nono_status_to_string NONO_OK");
        r.check(std::string(nono_status_to_string(NONO_ERROR_GENERIC)) == "NONO_ERROR_GENERIC", "nono_status_to_string NONO_ERROR_GENERIC");
        r.check(std::string(nono_status_to_string(NONO_ERROR_UNSUPPORTED)) == "NONO_ERROR_UNSUPPORTED", "nono_status_to_string NONO_ERROR_UNSUPPORTED");
        r.check(std::string(nono_status_to_string(NONO_ERROR_INVALID_PARAM)) == "NONO_ERROR_INVALID_PARAM", "nono_status_to_string NONO_ERROR_INVALID_PARAM");
        r.check(std::string(nono_status_to_string(NONO_ERROR_APPLY_FAILED)) == "NONO_ERROR_APPLY_FAILED", "nono_status_to_string NONO_ERROR_APPLY_FAILED");
        r.check(std::string(nono_status_to_string(NONO_ERROR_PERMISSION_DENIED)) == "NONO_ERROR_PERMISSION_DENIED", "nono_status_to_string NONO_ERROR_PERMISSION_DENIED");
        r.check(std::string(nono_status_to_string(NONO_ERROR_ALREADY_APPLIED)) == "NONO_ERROR_ALREADY_APPLIED", "nono_status_to_string NONO_ERROR_ALREADY_APPLIED");

        const char* bname = nono_get_backend_name();
        r.check(bname != nullptr && std::strlen(bname) > 0, "nono_get_backend_name returns non-empty string");

        nono_capability_set_free(caps);
    }

    {
        SandboxPolicy policy;
        policy.add_read_path("/usr/lib")
              .add_write_path("/tmp/cache")
              .add_device("/dev/kfd")
              .set_network_access(NetworkAccess::LoopbackOnly)
              .set_bind_port(9000);

        nono_capability_set* caps = nono_capability_set_new();
        r.check(caps != nullptr, "caps allocated for translation test");

        nono_status s = SandboxEngine::policy_to_nono_capabilities(policy, caps);
        r.check(s == NONO_OK, "policy_to_nono_capabilities succeeds");

        nono_capability_set_free(caps);
    }

    {
        auto platform_engine = SandboxEngine::create_for_platform();
        r.check(platform_engine != nullptr, "create_for_platform returns valid instance");
        r.check(std::strlen(platform_engine->get_backend_name()) > 0, "platform engine has valid backend name");

        EngineCapabilities caps = platform_engine->get_capabilities();
        r.check(caps.backend != EngineBackend::None, "engine capabilities has non-None backend");

        auto default_engine = SandboxEngine::create_default();
        r.check(default_engine != nullptr, "create_default returns valid instance");

        auto nono_engine = SandboxEngine::create_nono_ffi_engine();
        r.check(nono_engine != nullptr, "create_nono_ffi_engine returns valid instance");

        std::string desc_auto = SandboxEngine::get_platform_engine_description(SandboxMode::Auto);
        r.check(!desc_auto.empty(), "get_platform_engine_description(Auto) non-empty");

        std::string desc_disabled = SandboxEngine::get_platform_engine_description(SandboxMode::Disabled);
        r.check(desc_disabled == "disabled (by configuration)", "get_platform_engine_description(Disabled) matches exact string");

        std::string desc_enforced = SandboxEngine::get_platform_engine_description(SandboxMode::Enforced);
        r.check(!desc_enforced.empty(), "get_platform_engine_description(Enforced) non-empty");

        std::string desc_learn = SandboxEngine::get_platform_engine_description(SandboxMode::Learn);
        r.check(desc_learn.find("learning mode") != std::string::npos, "get_platform_engine_description(Learn) contains learning mode");
    }

    {
        nono_capability_set* caps = nono_capability_set_new();
        nono_capability_add_fs_read(caps, "/usr/include");
        nono_capability_add_fs_read(caps, "/usr/lib");
        nono_capability_add_fs_write(caps, "/tmp/cache");
        nono_capability_add_device(caps, "/dev/dri");
        nono_capability_set_network_loopback(caps, true);
        nono_capability_set_bind_port(caps, 8088);

        r.check(nono_capability_get_read_path_count(caps) == 2, "read_path_count is 2");
        r.check(std::string(nono_capability_get_read_path(caps, 0)) == "/usr/include", "read_path(0) matches");
        r.check(std::string(nono_capability_get_read_path(caps, 1)) == "/usr/lib", "read_path(1) matches");
        r.check(nono_capability_get_read_path(caps, 2) == nullptr, "read_path(2) out of bounds returns nullptr");

        r.check(nono_capability_get_write_path_count(caps) == 1, "write_path_count is 1");
        r.check(std::string(nono_capability_get_write_path(caps, 0)) == "/tmp/cache", "write_path(0) matches");

        r.check(nono_capability_get_device_count(caps) == 1, "device_count is 1");
        r.check(std::string(nono_capability_get_device(caps, 0)) == "/dev/dri", "device(0) matches");

        r.check(nono_capability_get_network_loopback(caps) == true, "network_loopback is true");
        r.check(nono_capability_get_network_egress(caps) == false, "network_egress is false");
        r.check(nono_capability_get_bind_port(caps) == 8088, "bind_port is 8088");

        SandboxPolicy policy = SandboxEngine::capabilities_to_policy(caps);
        r.check(policy.has_read_path("/usr/include"), "policy contains read_path /usr/include");
        r.check(policy.has_read_path("/usr/lib"), "policy contains read_path /usr/lib");
        r.check(policy.has_write_path("/tmp/cache"), "policy contains write_path /tmp/cache");
        r.check(policy.has_device("/dev/dri"), "policy contains device /dev/dri");
        r.check(policy.network_access == NetworkAccess::LoopbackOnly, "policy network_access is LoopbackOnly");
        r.check(policy.bind_port == 8088, "policy bind_port is 8088");

        nono_capability_set_free(caps);
    }

    r.report_summary("SandboxEngine");
    return r.exit_code();
}
