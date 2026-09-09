#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "lemon/model_types.h"
#include "lemon/sandbox/env_scrubber.h"

namespace lemon::sandbox {

enum class NetworkAccess {
    DenyAll,
    LoopbackOnly,
    Full
};

inline const char* network_access_to_string(NetworkAccess access) {
    switch (access) {
        case NetworkAccess::DenyAll:      return "deny_all";
        case NetworkAccess::LoopbackOnly: return "loopback_only";
        case NetworkAccess::Full:         return "full";
    }
    return "unknown";
}

enum class SandboxMode {
    Auto,
    Enforced,
    Disabled,
    ScrubbedOnly,
    Learn
};

inline const char* sandbox_mode_to_string(SandboxMode mode) {
    switch (mode) {
        case SandboxMode::Auto:         return "auto";
        case SandboxMode::Enforced:     return "enforced";
        case SandboxMode::Disabled:     return "disabled";
        case SandboxMode::ScrubbedOnly: return "scrubbed_only";
        case SandboxMode::Learn:        return "learn";
    }
    return "unknown";
}

inline SandboxMode parse_sandbox_mode(const std::string& str) {
    std::string lowered = str;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "enforced" || lowered == "strict") {
        return SandboxMode::Enforced;
    }
    if (lowered == "disabled" || lowered == "off" || lowered == "false" ||
        lowered == "none" || lowered == "0") {
        return SandboxMode::Disabled;
    }
    if (lowered == "scrubbed_only" || lowered == "scrubbed") {
        return SandboxMode::ScrubbedOnly;
    }
    if (lowered == "learn" || lowered == "profile" || lowered == "audit") {
        return SandboxMode::Learn;
    }
    return SandboxMode::Auto;
}

struct PathGrant {
    std::string path;
    bool write_allowed{false};

    PathGrant() = default;
    PathGrant(std::string p, bool write = false)
        : path(std::move(p)), write_allowed(write) {}

    static PathGrant read_only(std::string p) {
        return {std::move(p), false};
    }

    static PathGrant read_write(std::string p) {
        return {std::move(p), true};
    }

    bool operator==(const PathGrant& other) const noexcept {
        return path == other.path && write_allowed == other.write_allowed;
    }
};

struct SandboxPolicy {
    std::vector<PathGrant> path_grants;
    std::vector<std::string> device_grants;
    std::vector<std::string> allowed_env_vars;
    std::vector<std::pair<std::string, std::string>> explicit_env_vars;
    NetworkAccess network_access{NetworkAccess::LoopbackOnly};
    SandboxMode mode{SandboxMode::Auto};
    uint16_t bind_port{0};

    SandboxPolicy& add_path(const std::string& path, bool write_allowed = false) {
        if (!path.empty()) {
            path_grants.push_back({path, write_allowed});
        }
        return *this;
    }

    SandboxPolicy& add_read_path(const std::string& path) {
        return add_path(path, false);
    }

    SandboxPolicy& add_read_path(const char* path) {
        return add_path(path ? std::string(path) : std::string(), false);
    }

    SandboxPolicy& add_read_path(const std::filesystem::path& path) {
        return add_path(path.string(), false);
    }

    SandboxPolicy& add_write_path(const std::string& path) {
        return add_path(path, true);
    }

    SandboxPolicy& add_write_path(const char* path) {
        return add_path(path ? std::string(path) : std::string(), true);
    }

    SandboxPolicy& add_write_path(const std::filesystem::path& path) {
        return add_path(path.string(), true);
    }

    SandboxPolicy& add_device(const std::string& dev_path) {
        if (!dev_path.empty()) {
            device_grants.push_back(dev_path);
        }
        return *this;
    }

    SandboxPolicy& allow_env_var(const std::string& var_name) {
        if (!var_name.empty()) {
            allowed_env_vars.push_back(var_name);
        }
        return *this;
    }

    SandboxPolicy& allow_env_vars(const std::vector<std::string>& var_names) {
        for (const auto& var : var_names) {
            allow_env_var(var);
        }
        return *this;
    }

    SandboxPolicy& set_env_var(const std::string& key, const std::string& value) {
        if (!key.empty()) {
            explicit_env_vars.push_back({key, value});
        }
        return *this;
    }

    SandboxPolicy& set_network_access(NetworkAccess access) {
        network_access = access;
        return *this;
    }

    SandboxPolicy& set_mode(SandboxMode m) {
        mode = m;
        return *this;
    }

    SandboxPolicy& set_bind_port(uint16_t port) {
        bind_port = port;
        return *this;
    }

    bool has_read_path(const std::string& target_path) const {
        if (target_path.empty()) return false;
        std::filesystem::path target_p(target_path);
        std::string target_norm = target_p.lexically_normal().string();
        std::string target_gen = target_p.lexically_normal().generic_string();
        for (const auto& grant : path_grants) {
            if (grant.path == target_path) return true;
            std::filesystem::path gp(grant.path);
            if (gp.lexically_normal().string() == target_norm ||
                gp.lexically_normal().generic_string() == target_gen) {
                return true;
            }
#ifdef _WIN32
            if (_stricmp(gp.lexically_normal().string().c_str(), target_norm.c_str()) == 0 ||
                _stricmp(gp.lexically_normal().generic_string().c_str(), target_gen.c_str()) == 0) {
                return true;
            }
#endif
        }
        return false;
    }

    bool has_write_path(const std::string& target_path) const {
        if (target_path.empty()) return false;
        std::filesystem::path target_p(target_path);
        std::string target_norm = target_p.lexically_normal().string();
        std::string target_gen = target_p.lexically_normal().generic_string();
        for (const auto& grant : path_grants) {
            if (!grant.write_allowed) continue;
            if (grant.path == target_path) return true;
            std::filesystem::path gp(grant.path);
            if (gp.lexically_normal().string() == target_norm ||
                gp.lexically_normal().generic_string() == target_gen) {
                return true;
            }
#ifdef _WIN32
            if (_stricmp(gp.lexically_normal().string().c_str(), target_norm.c_str()) == 0 ||
                _stricmp(gp.lexically_normal().generic_string().c_str(), target_gen.c_str()) == 0) {
                return true;
            }
#endif
        }
        return false;
    }

    bool has_device(const std::string& dev) const {
        return std::find(device_grants.begin(), device_grants.end(), dev) != device_grants.end();
    }

    bool has_allowed_env(const std::string& var_name) const {
        return std::find(allowed_env_vars.begin(), allowed_env_vars.end(), var_name) != allowed_env_vars.end();
    }

    void normalize_paths() {
        std::vector<PathGrant> normalized;
        for (const auto& grant : path_grants) {
            if (grant.path.empty()) continue;
            std::filesystem::path p(grant.path);
            std::string norm_path = p.lexically_normal().string();

            auto it = std::find_if(normalized.begin(), normalized.end(),
                [&](const PathGrant& pg) {
                    std::filesystem::path pg_p(pg.path);
                    return pg_p.lexically_normal() == p.lexically_normal();
                });

            if (it != normalized.end()) {
                it->write_allowed = it->write_allowed || grant.write_allowed;
            } else {
                normalized.push_back({norm_path, grant.write_allowed});
            }
        }
        path_grants = std::move(normalized);
    }

    SandboxPolicy& merge(const SandboxPolicy& overrides) {
        if (overrides.mode != SandboxMode::Auto) {
            mode = overrides.mode;
        }
        if (overrides.network_access != NetworkAccess::LoopbackOnly) {
            network_access = overrides.network_access;
        }
        if (overrides.bind_port > 0) {
            bind_port = overrides.bind_port;
        }
        for (const auto& pg : overrides.path_grants) {
            if (pg.write_allowed) {
                add_write_path(pg.path);
            } else {
                add_read_path(pg.path);
            }
        }
        for (const auto& dg : overrides.device_grants) {
            add_device(dg);
        }
        for (const auto& ev : overrides.allowed_env_vars) {
            allow_env_var(ev);
        }
        for (const auto& kv : overrides.explicit_env_vars) {
            set_env_var(kv.first, kv.second);
        }
        normalize_paths();
        return *this;
    }

    std::string to_debug_string() const {
        std::ostringstream ss;
        ss << "mode=" << sandbox_mode_to_string(mode)
           << ", network=" << network_access_to_string(network_access);
        if (bind_port > 0) {
            ss << " (bind_port=" << bind_port << ")";
        }
        ss << ", paths=[" << path_grants.size() << " entries]"
           << ", devices=[";
        for (size_t i = 0; i < device_grants.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << device_grants[i];
        }
        ss << "], allowed_env=[" << allowed_env_vars.size() << " vars]";
        return ss.str();
    }

    std::string to_detailed_string() const {
        std::ostringstream ss;
        ss << "SandboxPolicy {\n"
           << "  mode: " << sandbox_mode_to_string(mode) << "\n"
           << "  network: " << network_access_to_string(network_access);
        if (bind_port > 0) {
            ss << " (bind_port: " << bind_port << ")";
        }
        ss << "\n  path_grants (" << path_grants.size() << "):\n";
        for (const auto& g : path_grants) {
            ss << "    " << (g.write_allowed ? "[RW] " : "[RO] ") << g.path << "\n";
        }
        ss << "  devices: [";
        for (size_t i = 0; i < device_grants.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << device_grants[i];
        }
        ss << "]\n  allowed_env: [";
        for (size_t i = 0; i < allowed_env_vars.size(); ++i) {
            if (i > 0) ss << ", ";
            ss << allowed_env_vars[i];
        }
        ss << "]\n}";
        return ss.str();
    }
};

inline void to_json(nlohmann::json& j, const PathGrant& p) {
    j = nlohmann::json{
        {"path", p.path},
        {"write_allowed", p.write_allowed}
    };
}

inline void from_json(const nlohmann::json& j, PathGrant& p) {
    j.at("path").get_to(p.path);
    if (j.contains("write_allowed")) {
        j.at("write_allowed").get_to(p.write_allowed);
    } else {
        p.write_allowed = false;
    }
}

inline void to_json(nlohmann::json& j, const NetworkAccess& na) {
    j = network_access_to_string(na);
}

inline void from_json(const nlohmann::json& j, NetworkAccess& na) {
    std::string s = j.get<std::string>();
    if (s == "deny_all" || s == "none") na = NetworkAccess::DenyAll;
    else if (s == "full" || s == "all") na = NetworkAccess::Full;
    else if (s == "loopback_only" || s == "loopback" || s == "localhost") na = NetworkAccess::LoopbackOnly;
    else throw std::invalid_argument("Unknown network_access mode: " + s);
}

inline void to_json(nlohmann::json& j, const SandboxMode& sm) {
    j = sandbox_mode_to_string(sm);
}

inline void from_json(const nlohmann::json& j, SandboxMode& sm) {
    std::string s = j.get<std::string>();
    std::string lowered = s;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "auto" || lowered == "default") sm = SandboxMode::Auto;
    else if (lowered == "enforced" || lowered == "strict") sm = SandboxMode::Enforced;
    else if (lowered == "disabled" || lowered == "off" || lowered == "false" || lowered == "none" || lowered == "0") sm = SandboxMode::Disabled;
    else if (lowered == "scrubbed_only" || lowered == "scrubbed") sm = SandboxMode::ScrubbedOnly;
    else if (lowered == "learn" || lowered == "profile" || lowered == "audit") sm = SandboxMode::Learn;
    else throw std::invalid_argument("Unknown sandbox mode: " + s);
}

inline void to_json(nlohmann::json& j, const SandboxPolicy& p) {
    j = nlohmann::json{
        {"mode", sandbox_mode_to_string(p.mode)},
        {"network_access", network_access_to_string(p.network_access)},
        {"bind_port", p.bind_port},
        {"path_grants", p.path_grants},
        {"device_grants", p.device_grants},
        {"allowed_env_vars", p.allowed_env_vars}
    };
    if (!p.explicit_env_vars.empty()) {
        nlohmann::json env_obj = nlohmann::json::object();
        for (const auto& [k, v] : p.explicit_env_vars) {
            env_obj[k] = v;
        }
        j["explicit_env_vars"] = env_obj;
    }
}

inline void from_json(const nlohmann::json& j, SandboxPolicy& p) {
    if (j.contains("mode")) {
        p.mode = j.at("mode").get<SandboxMode>();
    }
    if (j.contains("network_access")) {
        p.network_access = j.at("network_access").get<NetworkAccess>();
    }
    if (j.contains("bind_port")) {
        int port_val = j.at("bind_port").get<int>();
        if (port_val < 0 || port_val > 65535) {
            throw std::invalid_argument("bind_port out of valid range (0-65535): " + std::to_string(port_val));
        }
        p.bind_port = static_cast<uint16_t>(port_val);
    }
    if (j.contains("path_grants")) {
        j.at("path_grants").get_to(p.path_grants);
    }
    if (j.contains("read_paths")) {
        for (const auto& item : j["read_paths"]) {
            p.add_read_path(item.get<std::string>());
        }
    }
    if (j.contains("write_paths")) {
        for (const auto& item : j["write_paths"]) {
            p.add_write_path(item.get<std::string>());
        }
    }
    if (j.contains("device_grants")) {
        j.at("device_grants").get_to(p.device_grants);
    }
    if (j.contains("devices")) {
        for (const auto& item : j["devices"]) {
            p.add_device(item.get<std::string>());
        }
    }
    if (j.contains("allowed_env_vars")) {
        j.at("allowed_env_vars").get_to(p.allowed_env_vars);
    }
    if (j.contains("explicit_env_vars") && j["explicit_env_vars"].is_object()) {
        for (auto it = j["explicit_env_vars"].begin(); it != j["explicit_env_vars"].end(); ++it) {
            p.set_env_var(it.key(), it.value().get<std::string>());
        }
    }
}

inline bool validate_policy(const SandboxPolicy& policy, std::string* error_msg = nullptr) {
    auto fail = [&](const std::string& msg) {
        if (error_msg) *error_msg = msg;
        return false;
    };

    static const std::vector<std::string> banned_infrastructure_patterns = {
        "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN",
        "SSH_AUTH_SOCK", "SUDO_PASSWORD"
    };

    for (const auto& var : policy.allowed_env_vars) {
        std::string upper = var;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (upper.find("LEMONADE_") == 0) {
            if (upper.find("KEY") != std::string::npos ||
                upper.find("TOKEN") != std::string::npos ||
                upper.find("SECRET") != std::string::npos ||
                upper.find("PASSWORD") != std::string::npos ||
                upper.find("AUTH") != std::string::npos) {
                return fail("Policy allowed_env_vars contains forbidden Lemonade credential variable: " + var);
            }
        }

        for (const auto& pat : banned_infrastructure_patterns) {
            if (upper.find(pat) != std::string::npos) {
                return fail("Policy allowed_env_vars contains forbidden infrastructure secret pattern: " + var);
            }
        }
    }

    for (const auto& [k, v] : policy.explicit_env_vars) {
        std::string upper = k;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        if (upper.find("LEMONADE_") == 0) {
            if (upper.find("KEY") != std::string::npos ||
                upper.find("TOKEN") != std::string::npos ||
                upper.find("SECRET") != std::string::npos ||
                upper.find("PASSWORD") != std::string::npos ||
                upper.find("AUTH") != std::string::npos) {
                return fail("Policy explicit_env_vars contains forbidden Lemonade credential variable: " + k);
            }
        }
    }

    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#else
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_dir) {
            home = pw->pw_dir;
        }
    }
#endif
    std::string home_str = (home != nullptr) ? home : "";

    static const std::vector<std::string> banned_root_paths = {
        "/", "\\", "/etc", "\\etc", "/home", "\\home", "/root", "\\root", "/boot", "\\boot",
        "C:", "C:/", "C:\\", "C:/Users", "C:\\Users"
    };

    for (const auto& grant : policy.path_grants) {
        std::string p = grant.path;
        if (p.empty()) {
            return fail("Empty path grant is invalid");
        }

        std::filesystem::path fs_p(p);
        std::string normal_p = fs_p.lexically_normal().string();
        std::string generic_p = fs_p.lexically_normal().generic_string();

        std::error_code ec;
        std::filesystem::path canon_p = std::filesystem::weakly_canonical(fs_p, ec);
        std::string resolved_p = ec ? normal_p : canon_p.string();
        std::string resolved_gen = ec ? generic_p : canon_p.generic_string();

        if (p.find("..") != std::string::npos || normal_p.find("..") != std::string::npos || resolved_p.find("..") != std::string::npos) {
            return fail("Path grant contains directory traversal sequence: " + p);
        }

        auto check_broad_root = [&](const std::string& candidate_path) -> bool {
            for (const auto& root_dir : banned_root_paths) {
                if (candidate_path == root_dir || candidate_path == (root_dir + "/") || candidate_path == (root_dir + "\\")) {
                    return false;
                }
#ifdef _WIN32
                if (_stricmp(candidate_path.c_str(), root_dir.c_str()) == 0 ||
                    _stricmp(candidate_path.c_str(), (root_dir + "/").c_str()) == 0 ||
                    _stricmp(candidate_path.c_str(), (root_dir + "\\").c_str()) == 0) {
                    return false;
                }
#endif
            }

            if (!home_str.empty()) {
                std::filesystem::path hp(home_str);
                std::string norm_home = hp.lexically_normal().string();
                std::string gen_home = hp.lexically_normal().generic_string();
                if (candidate_path == norm_home || candidate_path == gen_home ||
                    candidate_path == (norm_home + "/") || candidate_path == (norm_home + "\\") ||
                    candidate_path == (gen_home + "/")) {
                    return false;
                }
#ifdef _WIN32
                if (_stricmp(candidate_path.c_str(), norm_home.c_str()) == 0 ||
                    _stricmp(candidate_path.c_str(), gen_home.c_str()) == 0 ||
                    _stricmp(candidate_path.c_str(), (norm_home + "/").c_str()) == 0 ||
                    _stricmp(candidate_path.c_str(), (norm_home + "\\").c_str()) == 0 ||
                    _stricmp(candidate_path.c_str(), (gen_home + "/").c_str()) == 0) {
                    return false;
                }
#endif
            }
            return true;
        };

        if (!check_broad_root(normal_p) || !check_broad_root(generic_p) ||
            !check_broad_root(resolved_p) || !check_broad_root(resolved_gen)) {
            return fail("Path grant targets broad system root or user home root directly: " + p);
        }
    }

    return true;
}

class PolicyPresets {
public:
    static std::vector<PathGrant> get_standard_system_paths() {
#ifdef _WIN32
        std::vector<PathGrant> paths;
        const char* sysroot = std::getenv("SystemRoot");
        if (!sysroot) sysroot = std::getenv("WINDIR");
        std::string win_dir = sysroot ? sysroot : "C:\\Windows";
        paths.push_back({win_dir + "\\System32", false});
        paths.push_back({win_dir + "\\SysWOW64", false});
        if (const char* pf = std::getenv("ProgramFiles")) {
            paths.push_back({pf, false});
        }
        if (const char* pfx86 = std::getenv("ProgramFiles(x86)")) {
            paths.push_back({pfx86, false});
        }
        if (const char* pd = std::getenv("ProgramData")) {
            paths.push_back({pd, false});
        }
        if (const char* tmp = std::getenv("TEMP")) {
            paths.push_back({tmp, true});
        } else if (const char* tmp2 = std::getenv("TMP")) {
            paths.push_back({tmp2, true});
        }
        return paths;
#elif defined(__APPLE__)
        return {
            {"/System/Library", false},
            {"/System/DriverKit", false},
            {"/usr/lib", false},
            {"/usr/share", false},
            {"/Library/Preferences", false},
            {"/dev/null", true},
            {"/dev/zero", false},
            {"/dev/urandom", false},
            {"/dev/random", false}
        };
#else
        return {
            {"/usr", false},
            {"/opt", false},
            {"/bin", false},
            {"/sbin", false},
            {"/lib", false},
            {"/lib64", false},
            {"/etc/ld.so.cache", false},
            {"/etc/ld.so.conf", false},
            {"/etc/ld.so.conf.d", false},
            {"/etc/alternatives", false},
            {"/proc/cpuinfo", false},
            {"/proc/meminfo", false},
            {"/proc/self", false},
            {"/sys/devices/system/cpu", false},
            {"/dev/null", true},
            {"/dev/zero", false},
            {"/dev/urandom", false},
            {"/dev/random", false},
            {"/dev/shm", true}
        };
#endif
    }

    static std::vector<std::string> get_standard_gpu_devices() {
#ifdef __APPLE__
        return {};
#elif defined(_WIN32)
        return {
            "\\\\.\\D3D",
            "\\\\.\\dxg"
        };
#else
        return {
            "/dev/dri",
            "/dev/kfd",
            "/dev/dxg",
            "/dev/nvidiactl",
            "/dev/nvidia-uvm",
            "/dev/nvidia0",
            "/dev/nvidia1",
            "/dev/nvidia2",
            "/dev/nvidia3",
            "/dev/nvidia4",
            "/dev/nvidia5",
            "/dev/nvidia6",
            "/dev/nvidia7",
            "/dev/nvidia8"
        };
#endif
    }

    static std::vector<std::string> get_standard_npu_devices() {
#if defined(_WIN32) || defined(__APPLE__)
        return {};
#else
        return {
            "/dev/accel",
            "/dev/amdxdna",
            "/sys/class/accel",
            "/dev/dri",
            "/dev/kfd"
        };
#endif
    }

    static std::vector<std::string> get_standard_allowed_env_vars() {
        std::vector<std::string> vars;
        const auto& common = EnvScrubber::common_allowed_env_vars();
        vars.reserve(common.size() + 10);
        vars.insert(vars.end(), common.begin(), common.end());
#ifndef _WIN32
        vars.push_back("LD_LIBRARY_PATH");
        vars.push_back("DYLD_LIBRARY_PATH");
        vars.push_back("LEMONADE_GGML_HIP_PATH");
        vars.push_back("XRT_LOG_LEVEL");
        vars.push_back("XRT_TPC_LOG_LEVEL");
#endif
        return vars;
    }

    // Step 1: Base System Runtime
    static void apply_system_runtime(SandboxPolicy& policy) {
        for (const auto& sp : get_standard_system_paths()) {
            policy.path_grants.push_back(sp);
        }
        policy.allow_env_vars(get_standard_allowed_env_vars());
    }

    // Step 2: Additive Hardware Profile Preset
    static void apply_hardware_profile(
        SandboxPolicy& policy,
        DeviceType device_type,
        const std::string& backend_variant = "") {

        std::string variant_lower = backend_variant;
        std::transform(variant_lower.begin(), variant_lower.end(), variant_lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        bool is_gpu = (device_type == DEVICE_GPU) ||
                      variant_lower == "vulkan" ||
                      variant_lower == "rocm" ||
                      variant_lower == "cuda";
        bool is_npu = (device_type == DEVICE_NPU) ||
                      variant_lower == "npu";

        if (is_gpu) {
            for (const auto& dev : get_standard_gpu_devices()) {
                policy.add_device(dev);
            }
#ifdef _WIN32
            if (const char* localapp = std::getenv("LOCALAPPDATA")) {
                std::filesystem::path la(localapp);
                policy.add_write_path((la / "NVIDIA" / "GLCache").string());
                policy.add_write_path((la / "AMD" / "DxCache").string());
            }
#elif defined(__APPLE__)
            if (const char* home = std::getenv("HOME")) {
                std::filesystem::path h(home);
                policy.add_write_path((h / "Library" / "Caches" / "com.apple.metal").string());
            }
#else
            policy.add_read_path("/sys/bus/pci");
            policy.add_read_path("/sys/devices");
            policy.add_read_path("/sys/class/drm");

            if (const char* home = std::getenv("HOME")) {
                std::filesystem::path h(home);
                policy.add_write_path((h / ".cache" / "mesa_shader_cache").string());
            }

            if (variant_lower == "rocm") {
                policy.add_read_path("/opt/rocm");
            }
            if (variant_lower == "cuda") {
                policy.add_read_path("/opt/cuda");
                policy.add_read_path("/usr/local/cuda");
            }
            if (variant_lower == "vulkan") {
                policy.add_read_path("/etc/vulkan");
                policy.add_read_path("/usr/share/vulkan");
            }
#endif
        } else if (is_npu) {
            for (const auto& dev : get_standard_npu_devices()) {
                policy.add_device(dev);
            }
#ifndef _WIN32
            policy.add_read_path("/opt/xilinx");
            policy.add_read_path("/opt/amd");
            policy.add_read_path("/etc/xrt");
            policy.add_read_path("/sys/class/accel");
            policy.add_read_path("/sys/bus/pci");
            policy.add_read_path("/sys/devices");
#endif
        }
    }

    // Step 3: Backend Workload Assets
    static void apply_backend_workload(
        SandboxPolicy& policy,
        const std::string& executable,
        const std::string& model_path) {

        if (!executable.empty()) {
            policy.add_read_path(executable);
            std::filesystem::path ep(executable);
            if (ep.has_parent_path()) {
                policy.add_read_path(ep.parent_path().string());
            }
        }

        if (!model_path.empty()) {
            policy.add_read_path(model_path);
            std::filesystem::path mp(model_path);
            if (mp.has_parent_path()) {
                std::filesystem::path parent = mp.parent_path();
                const char* home = std::getenv("HOME");
#ifdef _WIN32
                if (!home) home = std::getenv("USERPROFILE");
#endif
                std::filesystem::path hf_root = (home != nullptr)
                    ? (std::filesystem::path(home) / ".cache" / "huggingface")
                    : std::filesystem::path();

                bool is_hf_root = false;
                if (!hf_root.empty()) {
                    if (parent.lexically_normal() == hf_root.lexically_normal() ||
                        parent.lexically_normal().generic_string() == hf_root.lexically_normal().generic_string()) {
                        is_hf_root = true;
                    }
#ifdef _WIN32
                    if (_stricmp(parent.lexically_normal().generic_string().c_str(),
                                 hf_root.lexically_normal().generic_string().c_str()) == 0) {
                        is_hf_root = true;
                    }
#endif
                }

                if (!is_hf_root) {
                    policy.add_read_path(parent.string());
                }
            }
        }
    }

    // Explicit network egress grant
    static void apply_network_egress(SandboxPolicy& policy) {
        policy.network_access = NetworkAccess::Full;
        policy.allow_env_vars({
            "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY",
            "http_proxy", "https_proxy", "all_proxy", "no_proxy",
            "SSL_CERT_FILE", "SSL_CERT_DIR", "REQUESTS_CA_BUNDLE", "CURL_CA_BUNDLE"
        });
    }

    // Convenience base profile builder for simple tests/fixtures
    static SandboxPolicy create_base_profile(
        int bind_port = 0,
        const std::string& executable = "",
        const std::string& model_path = "") {

        SandboxPolicy policy;
        policy.bind_port = bind_port;
        policy.network_access = NetworkAccess::LoopbackOnly;
        apply_system_runtime(policy);
        apply_backend_workload(policy, executable, model_path);
        return policy;
    }
};

} // namespace lemon::sandbox
