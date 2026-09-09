#pragma once

#include <lemon/sandbox/nono_ffi.h>
#include <lemon/sandbox/sandbox_policy.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lemon::sandbox {

enum class EngineBackend {
    None,
    Landlock,
    Seatbelt,
    NonoFFI,
    WindowsAppContainer,
    WindowsDegraded,
    FallbackStub
};

inline const char* engine_backend_to_string(EngineBackend backend) {
    switch (backend) {
        case EngineBackend::None:                return "none";
        case EngineBackend::Landlock:            return "landlock";
        case EngineBackend::Seatbelt:            return "seatbelt";
        case EngineBackend::NonoFFI:             return "nono_ffi";
        case EngineBackend::WindowsAppContainer: return "windows_appcontainer";
        case EngineBackend::WindowsDegraded:     return "windows_degraded";
        case EngineBackend::FallbackStub:        return "fallback_stub";
    }
    return "unknown";
}

enum class PlatformType {
    LinuxNative,
    LinuxWSL2,
    MacOS,
    WindowsNative,
    Unknown
};

inline const char* platform_type_to_string(PlatformType type) {
    switch (type) {
        case PlatformType::LinuxNative:   return "linux_native";
        case PlatformType::LinuxWSL2:     return "linux_wsl2";
        case PlatformType::MacOS:         return "macos";
        case PlatformType::WindowsNative: return "windows_native";
        case PlatformType::Unknown:       return "unknown";
    }
    return "unknown";
}

struct EngineCapabilities {
    bool supports_fs_read_isolation{false};
    bool supports_fs_write_isolation{false};
    bool supports_device_isolation{false};
    bool supports_network_isolation{false};
    bool supports_port_binding{false};
    EngineBackend backend{EngineBackend::None};
    std::string backend_name{"none"};
    std::string description;
};

class PlatformDetector {
public:
    static PlatformType detect_platform();
    static bool is_wsl2();
    static bool is_native_windows();
    static bool is_macos();
    static bool is_linux_native();
    static bool has_dxg_device();

    static PlatformType parse_platform(
        const std::string& osrelease,
        const std::string& proc_version,
        bool has_dxg,
        bool is_apple_compiled,
        bool is_win32_compiled);
};

class SandboxEngine {
public:
    virtual ~SandboxEngine() = default;

    virtual bool is_supported() const = 0;
    virtual bool is_kernel_enforced() const = 0;
    virtual EngineBackend get_backend() const = 0;
    virtual const char* get_backend_name() const = 0;
    virtual EngineCapabilities get_capabilities() const = 0;

    // Irreversible kernel application. Must be called in child process post-fork, pre-exec.
    virtual bool apply(const SandboxPolicy& policy, std::string* error_msg = nullptr) = 0;

    static std::unique_ptr<SandboxEngine> create_for_platform();

    static std::unique_ptr<SandboxEngine> create_default() {
        return create_for_platform();
    }

    static std::unique_ptr<SandboxEngine> create_nono_ffi_engine();
    static std::unique_ptr<SandboxEngine> create_fallback_stub_engine();

    static bool is_wsl2_environment();
    static bool is_platform_supported();
    static std::string get_platform_engine_description(SandboxMode mode = SandboxMode::Auto);

    static nono_status policy_to_nono_capabilities(
        const SandboxPolicy& policy,
        nono_capability_set* caps);

    static SandboxPolicy capabilities_to_policy(const nono_capability_set* caps);
};

} // namespace lemon::sandbox
