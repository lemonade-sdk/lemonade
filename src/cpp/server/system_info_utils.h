#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <sstream>
#include <system_error>
#include <string>
#include <utility>
#include <vector>

namespace lemon::system_info_detail {

inline const std::set<std::string>& cuda_supported_archs() {
    static const std::set<std::string> archs = {
        "sm_75",   // Turing       (RTX 20, GTX 16, T4, Quadro RTX)
        "sm_80",   // Ampere DC    (A100)
        "sm_86",   // Ampere       (RTX 30, A40, A6000, A4000)
        "sm_89",   // Ada Lovelace (RTX 40, L40, L4)
        "sm_90",   // Hopper       (H100, H200)
        "sm_100",  // Blackwell DC (B100, B200)
        "sm_120",  // Blackwell    (RTX 50)
        "sm_121",  // Blackwell    (GB10 / Thor SoC)
    };
    return archs;
}

// Map a CUDA Compute Capability "MAJOR.MINOR" string to the sm_XX token used
// in llamacpp-cuda release filenames. Keep the parsing behavior identical to
// the server path so this helper is the single implementation under test.
inline std::string compute_cap_to_sm(const std::string& compute_cap) {
    const size_t dot = compute_cap.find('.');
    if (dot == std::string::npos) return "";

    const std::string major = compute_cap.substr(0, dot);
    const std::string minor = compute_cap.substr(dot + 1);
    if (major.empty() || minor.empty()) return "";

    try {
        const int m = std::stoi(major);
        const int n = std::stoi(minor);
        return "sm_" + std::to_string(m * 10 + n);
    } catch (...) {
        return "";
    }
}

// Match a concrete device family against exact or trailing-X family tokens.
inline bool device_matches_constraint(
    const std::string& device_family,
    const std::set<std::string>& allowed_families) {
    if (allowed_families.empty()) {
        return true;
    }
    if (allowed_families.count(device_family) > 0) {
        return true;
    }

    for (const auto& allowed_family : allowed_families) {
        if (allowed_family.size() > 1 && allowed_family.back() == 'X') {
            const std::string prefix = allowed_family.substr(0, allowed_family.size() - 1);
            if (device_family.compare(0, prefix.size(), prefix) == 0) {
                return true;
            }
        }
    }
    return false;
}

// Infer CUDA architecture from an NVIDIA marketing name when compute_cap is
// unavailable. Only architectures with published llamacpp-cuda binaries are
// represented here.
inline std::string identify_cuda_arch_from_name(const std::string& device_name) {
    std::string name = device_name;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    static const std::vector<std::string> nvidia_ids = {
        "nvidia", "geforce", "rtx", "gtx", "quadro", "tesla", "titan",
        "a100", "a40", "a30", "a10", "h100", "h200", "b100", "b200",
        "l40", "gb10",
    };

    const bool is_nvidia = std::any_of(
        nvidia_ids.begin(), nvidia_ids.end(), [&](const std::string& id) {
            return name.find(id) != std::string::npos;
        });
    if (!is_nvidia) {
        return "";
    }

    // Data-center Blackwell must win before the generic "blackwell" keyword.
    if (name.find("b100") != std::string::npos ||
        name.find("b200") != std::string::npos) {
        return "sm_100";
    }

    static const std::vector<std::pair<std::string, std::vector<std::string>>> table = {
        {"sm_121", {"gb10"}},
        {"sm_100", {"b100", "b200"}},
        {"sm_120", {"blackwell", "rtx 50", "rtx50", "5090", "5080", "5070", "5060",
                    "rtx pro 6000", "rtx pro 5000", "rtx pro 4500", "rtx pro 4000",
                    "rtx pro 3500", "rtx pro 3000", "rtx pro 2000", "rtx pro 1000"}},
        {"sm_90",  {"h100", "h200"}},
        {"sm_89",  {"rtx 40", "rtx40", "4090", "4080", "4070", "4060", "l40", " l4"}},
        {"sm_80",  {"a100"}},
        {"sm_86",  {"rtx 30", "rtx30", "3090", "3080", "3070", "3060", "3050",
                    "a40", "a30", "a10", "a6000", "a5000", "a4000", "a2000"}},
        {"sm_75",  {"rtx 20", "rtx20", "2080", "2070", "2060",
                    "gtx 16", "gtx16", "1660", "1650",
                    "titan rtx", "quadro rtx", " t4"}},
    };

    for (const auto& [sm, keywords] : table) {
        for (const auto& keyword : keywords) {
            if (name.find(keyword) != std::string::npos) {
                return sm;
            }
        }
    }
    return "";
}

// Splits a dotted numeric version ("576.02", "575.51.03") into its parts. Empty
// when any part is not a plain decimal number, so "Unknown" or "N/A" never compares.
inline std::vector<int> parse_numeric_version(const std::string& version) {
    std::vector<int> parts;
    std::string part;
    std::stringstream ss(version);
    while (std::getline(ss, part, '.')) {
        if (part.empty() || part.size() > 9 ||
            !std::all_of(part.begin(), part.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return {};
        }
        parts.push_back(std::stoi(part));
    }
    return parts;
}

// True only when both versions parse and `installed` sorts before `minimum`. An
// unreadable version is never treated as too old, so detection gaps cannot block a
// working driver.
inline bool nvidia_driver_older_than(const std::string& installed,
                                     const std::string& minimum) {
    const auto have = parse_numeric_version(installed);
    const auto want = parse_numeric_version(minimum);
    if (have.empty() || want.empty()) {
        return false;
    }
    const size_t n = std::max(have.size(), want.size());
    for (size_t i = 0; i < n; ++i) {
        const int a = i < have.size() ? have[i] : 0;
        const int b = i < want.size() ? want[i] : 0;
        if (a != b) {
            return a < b;
        }
    }
    return false;
}

// Windows reports NVIDIA drivers in the Windows driver-store format, whose last five
// digits are NVIDIA's own version: 32.0.15.7602 -> 576.02. Versions that are not in
// that four-part form are returned unchanged.
inline std::string nvidia_driver_version_from_windows(const std::string& version) {
    const auto parts = parse_numeric_version(version);
    if (parts.size() != 4) {
        return version;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d%04d", parts[2], parts[3]);
    const std::string digits(buf);
    if (digits.size() < 5) {
        return version;
    }
    const std::string tail = digits.substr(digits.size() - 5);
    return std::to_string(std::stoi(tail.substr(0, 3))) + "." + tail.substr(3);
}

// Oldest NVIDIA driver that can run a backend's CUDA binaries. Empty when no minimum
// is enforced. llamacpp:cuda is built with CUDA 12.9 (lemonade-sdk/llama.cpp release
// workflow), and CUDA minor-version compatibility lets any 12.x build run on the
// CUDA 12.0 driver floor, so these are that floor rather than the 12.9 toolkit's own
// minimum. They must move if the build switches CUDA major versions.
inline std::string cuda_min_driver_version(const std::string& recipe,
                                           const std::string& backend,
                                           const std::string& os) {
    if (recipe == "llamacpp" && backend == "cuda") {
        if (os == "windows") return "528.33";
        if (os == "linux") return "525.60.13";
    }
    return "";
}

// KFD publishes a GPU's ISA as packed decimal (110501 -> gfx1151), where the minor and
// step fields are hex nibbles. Empty when the value is not a decimal integer; the CPU
// node's 0 yields the unmatchable "gfx000" rather than an error.
inline std::string gfx_target_version_to_arch(const std::string& gfx_target_version) {
    if (gfx_target_version.empty() ||
        !std::all_of(gfx_target_version.begin(), gfx_target_version.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return "";
    }

    int packed = 0;
    try {
        packed = std::stoi(gfx_target_version);
    } catch (const std::exception&) {
        return "";
    }

    char buf[16];
    std::snprintf(buf, sizeof(buf), "gfx%d%x%x",
                  packed / 10000, (packed / 100) % 100, packed % 100);
    return std::string(buf);
}

// Keeps the ISA visible next to the driver's marketing name, since users match it
// against the arch targets backends are published for. Either half may be missing:
// the marketing name needs a driver that reports one, and the ISA needs a
// recognizable gfx_target_version.
inline std::string gpu_display_name(const std::string& marketing_name,
                                    const std::string& arch) {
    if (marketing_name.empty()) {
        return arch;
    }
    if (arch.empty() || marketing_name == arch) {
        return marketing_name;
    }
    return marketing_name + " (" + arch + ")";
}

// Reads the amdgpu driver's own per-device accounting under `kfd_nodes_dir` (KFD
// topology) and `drm_dir` (/sys/class/drm). Both are world-readable, unlike the libdrm
// probe elsewhere in this file's callers, which needs O_RDWR on a render node and so
// answers wrongly for a service account outside the `render` group.
//
// Reports nothing unless exactly one GPU matches: two of the same ISA leave no way to say
// which one a launch lands on, and guessing would size against the wrong device.
inline bool rocm_device_memory_from_sysfs(const std::filesystem::path& kfd_nodes_dir,
                                          const std::filesystem::path& drm_dir,
                                          const std::string& arch,
                                          uint64_t& free_bytes,
                                          uint64_t& total_bytes) {
    namespace fs = std::filesystem;

    if (arch.empty()) {
        return false;
    }

    std::error_code ec;
    if (!fs::is_directory(kfd_nodes_dir, ec)) {
        return false;
    }

    std::string wanted = arch;
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::vector<std::string> matching_render_minors;
    for (fs::directory_iterator it(kfd_nodes_dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        std::ifstream props(it->path() / "properties");
        if (!props.is_open()) {
            continue;
        }

        std::string line;
        std::string gfx_target_version;
        std::string drm_render_minor;
        while (std::getline(props, line)) {
            const size_t space = line.find(' ');
            if (space == std::string::npos) {
                continue;
            }
            const std::string key = line.substr(0, space);
            std::string value = line.substr(space + 1);
            const size_t end_of_value = value.find_last_not_of(" \t\n\r");
            value = end_of_value == std::string::npos
                ? ""
                : value.substr(0, end_of_value + 1);

            if (key == "gfx_target_version") {
                gfx_target_version = value;
            } else if (key == "drm_render_minor") {
                drm_render_minor = value;
            }
        }

        // drm_render_minor -1 marks a GPU with no render node, which no userspace
        // runtime can open. The CPU node needs no special case: its packed version of 0
        // cannot match a real ISA.
        if (drm_render_minor.empty() || drm_render_minor == "-1") {
            continue;
        }
        if (gfx_target_version_to_arch(gfx_target_version) == wanted) {
            matching_render_minors.push_back(drm_render_minor);
        }
    }

    if (matching_render_minors.size() != 1) {
        return false;
    }

    auto read_u64 = [](const fs::path& path, uint64_t& value) {
        std::ifstream file(path);
        return file.is_open() && static_cast<bool>(file >> value);
    };

    const fs::path device_dir =
        drm_dir / ("renderD" + matching_render_minors.front()) / "device";
    uint64_t used = 0;
    uint64_t total = 0;
    if (!read_u64(device_dir / "mem_info_vram_used", used) ||
        !read_u64(device_dir / "mem_info_vram_total", total) ||
        total == 0 || used > total) {
        return false;
    }

    free_bytes = total - used;
    total_bytes = total;
    return true;
}

struct KfdGpuNode {
    long node_id = -1;
    int hip_index = -1;
    std::string gfx_target_version;
    std::string drm_render_minor;
    uint64_t unique_id = 0;
    uint32_t domain = 0;
    uint32_t location_id = 0;
};

// HIP numbers GPUs in KFD node order, skipping the CPU node. Directory iteration
// order is unspecified, so nodes are sorted by their numeric id first.
inline std::vector<KfdGpuNode> kfd_gpu_nodes(const std::filesystem::path& kfd_nodes_dir) {
    namespace fs = std::filesystem;

    std::error_code ec;
    std::vector<std::pair<long, fs::path>> nodes;
    for (fs::directory_iterator it(kfd_nodes_dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        try {
            nodes.emplace_back(std::stol(it->path().filename().string()), it->path());
        } catch (const std::exception&) {
        }
    }
    std::sort(nodes.begin(), nodes.end());

    std::vector<KfdGpuNode> gpus;
    for (const auto& node : nodes) {
        std::ifstream props(node.second / "properties");
        KfdGpuNode gpu;
        gpu.node_id = node.first;
        std::string key;
        std::string value;
        while (props >> key >> value) {
            try {
                if (key == "gfx_target_version") gpu.gfx_target_version = value;
                else if (key == "drm_render_minor") gpu.drm_render_minor = value;
                else if (key == "unique_id") gpu.unique_id = std::stoull(value);
                else if (key == "domain") gpu.domain = static_cast<uint32_t>(std::stoul(value));
                else if (key == "location_id") gpu.location_id = static_cast<uint32_t>(std::stoul(value));
            } catch (const std::exception&) {
            }
        }
        if (gpu.gfx_target_version.empty() || gpu.gfx_target_version == "0") {
            continue;
        }
        gpu.hip_index = static_cast<int>(gpus.size());
        gpus.push_back(std::move(gpu));
    }
    return gpus;
}

inline std::string pci_bus_id(uint32_t domain, uint32_t bus, uint32_t device, uint32_t function) {
    char text[16];
    std::snprintf(text, sizeof(text), "%04x:%02x:%02x.%x", domain & 0xffff, bus & 0xff,
                  device & 0x1f, function & 0x7);
    return text;
}

// KFD packs the PCI address as location_id = bus << 8 | device << 3 | function.
inline std::string kfd_pci_bus_id(const KfdGpuNode& gpu) {
    return pci_bus_id(gpu.domain, gpu.location_id >> 8, (gpu.location_id >> 3) & 0x1f,
                      gpu.location_id & 0x7);
}

// The UUID ROCr reports and ROCR_VISIBLE_DEVICES accepts: "GPU-" and unique_id as 16
// hex digits. Empty for a GPU without one, which ROCr reports as "GPU-XX".
inline std::string rocm_uuid(uint64_t unique_id) {
    if (unique_id == 0) return "";
    char text[24];
    std::snprintf(text, sizeof(text), "GPU-%016llx", static_cast<unsigned long long>(unique_id));
    return text;
}

struct HipProbeDevice {
    int hip_index = -1;
    std::string pci_bus_id;
    std::string name;
};

// The internal HIP probe prints one "index<TAB>pci_bus_id<TAB>name" line per device.
inline std::vector<HipProbeDevice> parse_hip_probe_output(const std::string& output) {
    std::vector<HipProbeDevice> devices;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t first = line.find('\t');
        const size_t second = first == std::string::npos ? first : line.find('\t', first + 1);
        if (second == std::string::npos) continue;
        HipProbeDevice device;
        try {
            device.hip_index = std::stoi(line.substr(0, first));
        } catch (const std::exception&) {
            continue;
        }
        device.pci_bus_id = line.substr(first + 1, second - first - 1);
        device.name = line.substr(second + 1);
        devices.push_back(std::move(device));
    }
    return devices;
}

// Matches on bus, device and function only: Windows reports no PCI domain.
inline int hip_index_for_pci_bus_id(const std::vector<HipProbeDevice>& devices,
                                    const std::string& pci_bus_id) {
    auto without_domain = [](std::string id) {
        std::transform(id.begin(), id.end(), id.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const size_t colon = id.find(':');
        return colon != std::string::npos && id.find(':', colon + 1) != std::string::npos
            ? id.substr(colon + 1) : id;
    };
    const std::string wanted = without_domain(pci_bus_id);
    for (const auto& device : devices) {
        if (!wanted.empty() && without_domain(device.pci_bus_id) == wanted) return device.hip_index;
    }
    return -1;
}

// A container given all of /dev/kfd needs this index to run on the GPU it was sized
// for. Empty when no GPU satisfies `runs_on`.
inline std::string hip_visible_device(const std::filesystem::path& kfd_nodes_dir,
                                      const std::function<bool(const std::string& arch)>& runs_on) {
    for (const auto& gpu : kfd_gpu_nodes(kfd_nodes_dir)) {
        if (runs_on(gfx_target_version_to_arch(gpu.gfx_target_version))) {
            return std::to_string(gpu.hip_index);
        }
    }
    return "";
}

// The properties file of the first GPU node of `arch`, or "" when none is present.
inline std::string kfd_gpu_properties(const std::filesystem::path& kfd_nodes_dir,
                                      const std::string& arch) {
    namespace fs = std::filesystem;

    if (arch.empty()) {
        return "";
    }
    std::error_code ec;
    for (fs::directory_iterator it(kfd_nodes_dir, ec), end; it != end && !ec;
         it.increment(ec)) {
        std::ifstream props(it->path() / "properties");
        const std::string text((std::istreambuf_iterator<char>(props)),
                               std::istreambuf_iterator<char>());
        std::istringstream lines(text);
        std::string key;
        std::string value;
        while (lines >> key >> value) {
            if (key == "gfx_target_version") {
                if (gfx_target_version_to_arch(value) == arch) {
                    return text;
                }
                break;
            }
        }
    }
    return "";
}

}  // namespace lemon::system_info_detail
