#include "lemon/sandbox/sandbox_engine.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/utsname.h>
#endif

namespace lemon::sandbox {

PlatformType PlatformDetector::parse_platform(
    const std::string& osrelease,
    const std::string& proc_version,
    bool has_dxg,
    bool is_apple_compiled,
    bool is_win32_compiled) {

    if (is_win32_compiled) {
        return PlatformType::WindowsNative;
    }
    if (is_apple_compiled) {
        return PlatformType::MacOS;
    }

    std::string os_lower = osrelease;
    std::transform(os_lower.begin(), os_lower.end(), os_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string proc_lower = proc_version;
    std::transform(proc_lower.begin(), proc_lower.end(), proc_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (os_lower.find("wsl") != std::string::npos ||
        proc_lower.find("microsoft") != std::string::npos ||
        proc_lower.find("wsl") != std::string::npos ||
        has_dxg) {
        return PlatformType::LinuxWSL2;
    }

    if (!osrelease.empty() || !proc_version.empty()) {
        return PlatformType::LinuxNative;
    }

    return PlatformType::Unknown;
}

PlatformType PlatformDetector::detect_platform() {
#if defined(_WIN32)
    return PlatformType::WindowsNative;
#elif defined(__APPLE__)
    return PlatformType::MacOS;
#elif defined(__linux__)
    std::string osrelease;
    struct utsname uts;
    if (uname(&uts) == 0) {
        osrelease = uts.release;
    }

    std::string proc_version;
    std::ifstream pv("/proc/version");
    if (pv.is_open()) {
        std::getline(pv, proc_version);
    }

    return parse_platform(osrelease, proc_version, has_dxg_device(), false, false);
#else
    return PlatformType::Unknown;
#endif
}

bool PlatformDetector::is_wsl2() {
    return detect_platform() == PlatformType::LinuxWSL2;
}

bool PlatformDetector::is_native_windows() {
    return detect_platform() == PlatformType::WindowsNative;
}

bool PlatformDetector::is_macos() {
    return detect_platform() == PlatformType::MacOS;
}

bool PlatformDetector::is_linux_native() {
    return detect_platform() == PlatformType::LinuxNative;
}

bool PlatformDetector::has_dxg_device() {
#if defined(__linux__)
    std::ifstream dev("/dev/dxg");
    return dev.good();
#else
    return false;
#endif
}

class FallbackStubEngine : public SandboxEngine {
public:
    bool is_supported() const override {
        return false;
    }

    bool is_kernel_enforced() const override {
        return false;
    }

    EngineBackend get_backend() const override {
        return EngineBackend::FallbackStub;
    }

    const char* get_backend_name() const override {
        return "fallback_stub";
    }

    EngineCapabilities get_capabilities() const override {
        EngineCapabilities caps;
        caps.supports_fs_read_isolation = false;
        caps.supports_fs_write_isolation = false;
        caps.supports_device_isolation = false;
        caps.supports_network_isolation = false;
        caps.supports_port_binding = false;
        caps.backend = EngineBackend::FallbackStub;
        caps.backend_name = "fallback_stub";
        caps.description = "Fallback escape hatch for unconfined execution (secret scrubbing only)";
        return caps;
    }

    bool apply(const SandboxPolicy& policy, std::string* error_msg = nullptr) override {
        if (policy.mode == SandboxMode::Disabled || policy.mode == SandboxMode::ScrubbedOnly) {
            return true;
        }
        if (policy.mode == SandboxMode::Enforced) {
            if (error_msg) {
                *error_msg = "Enforced kernel sandboxing requested but sandbox engine is in fallback mode";
            }
            return false;
        }
        return true;
    }
};

#ifdef _WIN32
class WindowsAppContainerEngine : public SandboxEngine {
public:
    bool is_supported() const override {
        return true;
    }

    bool is_kernel_enforced() const override {
        return true;
    }

    EngineBackend get_backend() const override {
        return EngineBackend::WindowsAppContainer;
    }

    const char* get_backend_name() const override {
        return "windows_appcontainer";
    }

    EngineCapabilities get_capabilities() const override {
        EngineCapabilities caps;
        caps.supports_fs_read_isolation = true;
        caps.supports_fs_write_isolation = true;
        caps.supports_device_isolation = false;
        caps.supports_network_isolation = true;
        caps.supports_port_binding = false;
        caps.backend = EngineBackend::WindowsAppContainer;
        caps.backend_name = "windows_appcontainer";
        caps.description = "Native Windows AppContainer, Job Object, and mitigation policy isolation engine";
        return caps;
    }

    bool apply(const SandboxPolicy& policy, std::string* error_msg = nullptr) override {
        if (policy.mode == SandboxMode::Disabled || policy.mode == SandboxMode::ScrubbedOnly) {
            return true;
        }
        return true;
    }
};
#endif

class NonoFFIEngine : public SandboxEngine {
public:
    bool is_supported() const override {
        return nono_is_supported();
    }

    bool is_kernel_enforced() const override {
#ifdef __APPLE__
        std::string cand1 = lemon::utils::get_downloaded_bin_dir() + "/lemonade-sandbox-exec";
        return std::filesystem::exists(cand1) ||
               std::filesystem::exists("/usr/local/bin/lemonade-sandbox-exec") ||
               std::filesystem::exists("/opt/homebrew/bin/lemonade-sandbox-exec");
#else
        return nono_is_supported();
#endif
    }

    EngineBackend get_backend() const override {
        return EngineBackend::NonoFFI;
    }

    const char* get_backend_name() const override {
#ifdef __APPLE__
        std::string cand1 = lemon::utils::get_downloaded_bin_dir() + "/lemonade-sandbox-exec";
        bool has_trampoline = std::filesystem::exists(cand1) ||
                              std::filesystem::exists("/usr/local/bin/lemonade-sandbox-exec") ||
                              std::filesystem::exists("/opt/homebrew/bin/lemonade-sandbox-exec");
        return has_trampoline ? "nono-seatbelt-trampoline" : "macos_scrubbed_only";
#else
        return nono_get_backend_name();
#endif
    }

    EngineCapabilities get_capabilities() const override {
        EngineCapabilities caps;
#ifdef __APPLE__
        std::string cand1 = lemon::utils::get_downloaded_bin_dir() + "/lemonade-sandbox-exec";
        bool has_trampoline = std::filesystem::exists(cand1) ||
                              std::filesystem::exists("/usr/local/bin/lemonade-sandbox-exec") ||
                              std::filesystem::exists("/opt/homebrew/bin/lemonade-sandbox-exec");

        caps.supports_fs_read_isolation = has_trampoline;
        caps.supports_fs_write_isolation = has_trampoline;
        caps.supports_device_isolation = has_trampoline;
        caps.supports_network_isolation = has_trampoline;
        caps.supports_port_binding = has_trampoline;
        caps.backend = EngineBackend::NonoFFI;
        caps.backend_name = has_trampoline ? "nono-seatbelt-trampoline" : "macos_scrubbed_only";
        caps.description = has_trampoline ? "nono macOS Seatbelt sandboxing via lemonade-sandbox-exec trampoline"
                                          : "macOS posix_spawn environment scrubbing only";
#else
        caps.supports_fs_read_isolation = true;
        caps.supports_fs_write_isolation = true;
        caps.supports_device_isolation = true;
        caps.supports_network_isolation = true;
        caps.supports_port_binding = true;
        caps.backend = EngineBackend::NonoFFI;
        caps.backend_name = nono_get_backend_name();
        caps.description = "nono kernel sandboxing engine via C FFI";
#endif
        return caps;
    }

    bool apply(const SandboxPolicy& policy, std::string* error_msg = nullptr) override {
        if (policy.mode == SandboxMode::Disabled || policy.mode == SandboxMode::ScrubbedOnly) {
            return true;
        }

        nono_capability_set* caps = nono_capability_set_new();
        if (!caps) {
            if (error_msg) *error_msg = "Failed to allocate nono_capability_set";
            return false;
        }

        nono_status status = SandboxEngine::policy_to_nono_capabilities(policy, caps);
        if (status != NONO_OK) {
            if (error_msg) *error_msg = nono_get_last_error();
            nono_capability_set_free(caps);
            return false;
        }

        status = nono_sandbox_apply(caps);
        nono_capability_set_free(caps);

        if (status != NONO_OK) {
            if (policy.mode == SandboxMode::Auto && status == NONO_ERROR_UNSUPPORTED) {
                return true;
            }
            if (error_msg) {
                *error_msg = nono_get_last_error();
                if (error_msg->empty()) {
                    *error_msg = nono_status_to_string(status);
                }
            }
            return false;
        }
        return true;
    }
};

bool SandboxEngine::is_wsl2_environment() {
    return PlatformDetector::is_wsl2();
}

bool SandboxEngine::is_platform_supported() {
#ifdef _WIN32
    return true;
#else
    return nono_is_supported();
#endif
}

nono_status SandboxEngine::policy_to_nono_capabilities(
    const SandboxPolicy& policy,
    nono_capability_set* caps) {

    if (!caps) return NONO_ERROR_INVALID_PARAM;

    for (const auto& grant : policy.path_grants) {
        if (grant.path.empty()) continue;
        std::error_code ec;
        std::filesystem::path p = std::filesystem::weakly_canonical(grant.path, ec);
        std::string resolved_path = ec ? grant.path : p.string();

        nono_status s;
        if (grant.write_allowed) {
            s = nono_capability_add_fs_write(caps, resolved_path.c_str());
        } else {
            s = nono_capability_add_fs_read(caps, resolved_path.c_str());
            if (s == NONO_OK && p.has_parent_path()) {
                std::filesystem::path parent = p.parent_path();
                if (parent.filename() == "snapshots" && parent.has_parent_path()) {
                    std::filesystem::path blobs = parent.parent_path() / "blobs";
                    if (std::filesystem::exists(blobs, ec)) {
                        nono_capability_add_fs_read(caps, blobs.string().c_str());
                    }
                }
            }
        }
        if (s != NONO_OK) return s;
    }

    for (const auto& dev : policy.device_grants) {
        if (dev.empty()) continue;
        nono_status s = nono_capability_add_device(caps, dev.c_str());
        if (s != NONO_OK) return s;
    }

    switch (policy.network_access) {
        case NetworkAccess::DenyAll:
            nono_capability_set_network_egress(caps, false);
            nono_capability_set_network_loopback(caps, false);
            break;
        case NetworkAccess::LoopbackOnly:
            nono_capability_set_network_egress(caps, false);
            nono_capability_set_network_loopback(caps, true);
            break;
        case NetworkAccess::Full:
            nono_capability_set_network_egress(caps, true);
            nono_capability_set_network_loopback(caps, true);
            break;
    }

    if (policy.bind_port > 0) {
        nono_capability_set_bind_port(caps, policy.bind_port);
    }

    return NONO_OK;
}

SandboxPolicy SandboxEngine::capabilities_to_policy(const nono_capability_set* caps) {
    SandboxPolicy policy;
    if (!caps) return policy;

    size_t r_count = nono_capability_get_read_path_count(caps);
    for (size_t i = 0; i < r_count; ++i) {
        const char* p = nono_capability_get_read_path(caps, i);
        if (p && p[0] != '\0') {
            policy.add_read_path(p);
        }
    }

    size_t w_count = nono_capability_get_write_path_count(caps);
    for (size_t i = 0; i < w_count; ++i) {
        const char* p = nono_capability_get_write_path(caps, i);
        if (p && p[0] != '\0') {
            policy.add_write_path(p);
        }
    }

    size_t d_count = nono_capability_get_device_count(caps);
    for (size_t i = 0; i < d_count; ++i) {
        const char* d = nono_capability_get_device(caps, i);
        if (d && d[0] != '\0') {
            policy.add_device(d);
        }
    }

    bool egress = nono_capability_get_network_egress(caps);
    bool loopback = nono_capability_get_network_loopback(caps);
    if (egress) {
        policy.set_network_access(NetworkAccess::Full);
    } else if (loopback) {
        policy.set_network_access(NetworkAccess::LoopbackOnly);
    } else {
        policy.set_network_access(NetworkAccess::DenyAll);
    }

    uint16_t port = nono_capability_get_bind_port(caps);
    if (port > 0) {
        policy.set_bind_port(port);
    }

    return policy;
}

std::unique_ptr<SandboxEngine> SandboxEngine::create_fallback_stub_engine() {
    return std::make_unique<FallbackStubEngine>();
}

std::unique_ptr<SandboxEngine> SandboxEngine::create_nono_ffi_engine() {
    return std::make_unique<NonoFFIEngine>();
}

std::unique_ptr<SandboxEngine> SandboxEngine::create_for_platform() {
#ifdef _WIN32
    return std::make_unique<WindowsAppContainerEngine>();
#else
    if (nono_is_supported()) {
        return create_nono_ffi_engine();
    }
    return create_fallback_stub_engine();
#endif
}

std::string SandboxEngine::get_platform_engine_description(SandboxMode mode) {
    if (mode == SandboxMode::Disabled) {
        return "disabled (by configuration)";
    }
    if (mode == SandboxMode::Learn) {
        return "learning mode (in-process nono capability profiling)";
    }
#ifdef _WIN32
    return std::string("windows_appcontainer (") + (mode == SandboxMode::Enforced ? "enforced" : "active") + ")";
#else
    if (nono_is_supported()) {
        std::string name = nono_get_backend_name();
        return name + " (" + (mode == SandboxMode::Enforced ? "enforced" : "active") + ")";
    }
    return "degraded (unconfined fallback mode, secret scrubbing active)";
#endif
}

} // namespace lemon::sandbox
