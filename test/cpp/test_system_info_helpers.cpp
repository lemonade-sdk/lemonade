// C++ coverage for the pure SystemInfo CUDA mapping and device-family matching
// helpers. These helpers are used directly by system_info.cpp, so the test covers
// the production implementation instead of a Python replica.
//
// Build: cmake --build --preset default --target test_system_info_helpers
// Run:   ctest --test-dir build -R '^SystemInfoHelpersTest$' --output-on-failure

#include "system_info_utils.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

using lemon::system_info_detail::compute_cap_to_sm;
using lemon::system_info_detail::cuda_supported_archs;
using lemon::system_info_detail::device_matches_constraint;
using lemon::system_info_detail::gfx_target_version_to_arch;
using lemon::system_info_detail::identify_cuda_arch_from_name;
using lemon::system_info_detail::rocm_device_memory_from_sysfs;

static bool expect_string(const char* name,
                          const std::string& actual,
                          const std::string& expected) {
    const bool ok = actual == expected;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  '%s'\n  want: '%s'\n",
                    actual.c_str(), expected.c_str());
    }
    return ok;
}

static bool expect_bool(const char* name, bool actual, bool expected) {
    const bool ok = actual == expected;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  %s\n  want: %s\n",
                    actual ? "true" : "false",
                    expected ? "true" : "false");
    }
    return ok;
}

static bool expect_membership(const char* name,
                              const std::string& arch,
                              bool expected) {
    const bool actual = cuda_supported_archs().count(arch) > 0;
    return expect_bool(name, actual, expected);
}

namespace fs = std::filesystem;

// A throwaway copy of the two world-readable sysfs trees the ROCm memory probe reads,
// so the topology walk is exercised with no GPU present.
class FakeSysfs {
public:
    explicit FakeSysfs(const std::string& tag)
        : root_(fs::temp_directory_path() / ("lemonade_sysfs_" + tag)) {
        fs::remove_all(root_);
        fs::create_directories(kfd_nodes());
        fs::create_directories(drm());
    }

    ~FakeSysfs() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    FakeSysfs(const FakeSysfs&) = delete;
    FakeSysfs& operator=(const FakeSysfs&) = delete;

    fs::path kfd_nodes() const { return root_ / "kfd_nodes"; }
    fs::path drm() const { return root_ / "drm"; }

    void add_node(const std::string& node_id,
                  const std::string& gfx_target_version,
                  const std::string& drm_render_minor) const {
        const fs::path node = kfd_nodes() / node_id;
        fs::create_directories(node);
        std::ofstream props(node / "properties");
        props << "cpu_cores_count 0\n"
              << "gfx_target_version " << gfx_target_version << "\n"
              << "drm_render_minor " << drm_render_minor << "\n";
    }

    void add_memory(const std::string& drm_render_minor,
                    const std::string& used,
                    const std::string& total) const {
        const fs::path device = drm() / ("renderD" + drm_render_minor) / "device";
        fs::create_directories(device);
        std::ofstream(device / "mem_info_vram_used") << used << "\n";
        std::ofstream(device / "mem_info_vram_total") << total << "\n";
    }

private:
    fs::path root_;
};

static bool expect_device_memory(const char* name,
                                 const FakeSysfs& sysfs,
                                 const std::string& arch,
                                 bool expected_found,
                                 uint64_t expected_free = 0,
                                 uint64_t expected_total = 0) {
    uint64_t free_bytes = 0;
    uint64_t total_bytes = 0;
    const bool found = rocm_device_memory_from_sysfs(
        sysfs.kfd_nodes(), sysfs.drm(), arch, free_bytes, total_bytes);

    bool ok = found == expected_found;
    if (ok && found) {
        ok = free_bytes == expected_free && total_bytes == expected_total;
    }
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  found=%s free=%llu total=%llu\n"
                    "  want: found=%s free=%llu total=%llu\n",
                    found ? "true" : "false",
                    static_cast<unsigned long long>(free_bytes),
                    static_cast<unsigned long long>(total_bytes),
                    expected_found ? "true" : "false",
                    static_cast<unsigned long long>(expected_free),
                    static_cast<unsigned long long>(expected_total));
    }
    return ok;
}

int main() {
    int failures = 0;

    const std::vector<std::pair<std::string, std::string>> compute_cap_cases = {
        {"7.5", "sm_75"},
        {"8.0", "sm_80"},
        {"8.6", "sm_86"},
        {"8.9", "sm_89"},
        {"9.0", "sm_90"},
        {"10.0", "sm_100"},
        {"12.0", "sm_120"},
        {"12.1", "sm_121"},
        {"6.1", "sm_61"},
        {"5.0", "sm_50"},
    };
    for (const auto& [input, expected] : compute_cap_cases) {
        const std::string name = "compute_cap " + input;
        failures += !expect_string(name.c_str(), compute_cap_to_sm(input), expected);
    }

    for (const char* input : {"", "86", "8", ".", "abc", "8.x"}) {
        const std::string name = std::string("invalid compute_cap ") + input;
        failures += !expect_string(name.c_str(), compute_cap_to_sm(input), "");
    }

    for (const char* cap : {"7.5", "8.0", "8.6", "8.9", "9.0", "10.0", "12.0", "12.1"}) {
        const std::string arch = compute_cap_to_sm(cap);
        const std::string name = "supported CUDA arch " + arch;
        failures += !expect_membership(name.c_str(), arch, true);
    }
    for (const char* cap : {"6.1", "7.0", "5.0"}) {
        const std::string arch = compute_cap_to_sm(cap);
        const std::string name = "unsupported CUDA arch " + arch;
        failures += !expect_membership(name.c_str(), arch, false);
    }

    const std::vector<std::pair<std::string, std::string>> gpu_name_cases = {
        {"NVIDIA GeForce RTX 3080", "sm_86"},
        {"NVIDIA GeForce RTX 4090", "sm_89"},
        {"NVIDIA GeForce RTX 2080 Ti", "sm_75"},
        {"NVIDIA GeForce RTX 5090", "sm_120"},
        {"NVIDIA GeForce RTX 5080", "sm_120"},
        {"NVIDIA GTX 1660 Super", "sm_75"},
        {"NVIDIA H100 PCIe", "sm_90"},
        {"NVIDIA A100-SXM4-80GB", "sm_80"},
        {"NVIDIA A10", "sm_86"},
        {"NVIDIA A40", "sm_86"},
        {"NVIDIA GB10", "sm_121"},
        {"GB10", "sm_121"},
        {"NVIDIA RTX PRO 3000 Blackwell Generation Laptop GPU", "sm_120"},
        {"NVIDIA RTX PRO 6000 Blackwell", "sm_120"},
        {"NVIDIA RTX PRO 4000 Blackwell", "sm_120"},
        {"NVIDIA RTX PRO 1000", "sm_120"},
        {"NVIDIA B200", "sm_100"},
        {"NVIDIA B200 (Blackwell)", "sm_100"},
        {"NVIDIA B100", "sm_100"},
        {"NVIDIA B100 Blackwell", "sm_100"},
        {"NVIDIA B200 Blackwell", "sm_100"},
    };
    for (const auto& [input, expected] : gpu_name_cases) {
        const std::string name = "GPU name " + input;
        failures += !expect_string(
            name.c_str(), identify_cuda_arch_from_name(input), expected);
    }

    for (const char* input : {
             "AMD Radeon RX 7900 XTX", "Intel Arc A770", "Apple M3", "",
             "NVIDIA GTX 1080 Ti", "NVIDIA GTX 980", "NVIDIA Tesla K80"}) {
        const std::string name = std::string("unsupported GPU name ") + input;
        failures += !expect_string(
            name.c_str(), identify_cuda_arch_from_name(input), "");
    }

    failures += !expect_bool(
        "gfx1103 matches gfx110X",
        device_matches_constraint("gfx1103", {"gfx110X"}), true);
    failures += !expect_bool(
        "gfx1201 matches gfx120X",
        device_matches_constraint("gfx1201", {"gfx120X"}), true);
    failures += !expect_bool(
        "gfx1151 exact match",
        device_matches_constraint("gfx1151", {"gfx1151"}), true);
    failures += !expect_bool(
        "gfx1152 exact match",
        device_matches_constraint("gfx1152", {"gfx1152"}), true);
    failures += !expect_bool(
        "gfx1151 does not match gfx110X",
        device_matches_constraint("gfx1151", {"gfx110X"}), false);
    failures += !expect_bool(
        "empty allowed family matches everything",
        device_matches_constraint("gfx1151", {}), true);
    failures += !expect_bool(
        "matches one of multiple wildcard families",
        device_matches_constraint("gfx1103", {"gfx103X", "gfx110X"}), true);
    failures += !expect_bool(
        "matches second wildcard family",
        device_matches_constraint("gfx1201", {"gfx110X", "gfx120X"}), true);
    failures += !expect_bool(
        "does not match any wildcard family",
        device_matches_constraint("gfx1151", {"gfx103X", "gfx110X"}), false);

    // KFD publishes a GPU's ISA as packed decimal, with hex minor and step nibbles.
    failures += !expect_string(
        "gfx_target_version 110501 is gfx1151",
        gfx_target_version_to_arch("110501"), "gfx1151");
    failures += !expect_string(
        "gfx_target_version 90010 is gfx90a, whose step is a hex nibble",
        gfx_target_version_to_arch("90010"), "gfx90a");
    failures += !expect_string(
        "gfx_target_version 120001 is gfx1201",
        gfx_target_version_to_arch("120001"), "gfx1201");
    failures += !expect_string(
        "gfx_target_version 90402 is gfx942",
        gfx_target_version_to_arch("90402"), "gfx942");
    // Preserved from the original inline conversion: 0 formats rather than erroring, and
    // the result cannot collide with a real ISA, so the CPU node needs no special case.
    failures += !expect_string(
        "the CPU node's zero version yields an unmatchable ISA",
        gfx_target_version_to_arch("0"), "gfx000");
    failures += !expect_string(
        "an already-formatted ISA is not a packed version",
        gfx_target_version_to_arch("gfx1151"), "");
    failures += !expect_string(
        "an empty version has no ISA",
        gfx_target_version_to_arch(""), "");

    {
        // 27.07 of 32.0 GiB free: the reading from the gfx1151 merge-queue failures.
        FakeSysfs sysfs("single");
        sysfs.add_node("0", "0", "-1");
        sysfs.add_node("1", "110501", "128");
        sysfs.add_memory("128", "5292500582", "34359738368");
        failures += !expect_device_memory(
            "a single matching GPU reports its own free and total bytes",
            sysfs, "gfx1151", true, 34359738368ULL - 5292500582ULL, 34359738368ULL);
        failures += !expect_device_memory(
            "an ISA absent from the topology is not reported",
            sysfs, "gfx942", false);
        failures += !expect_device_memory(
            "an empty ISA is not reported",
            sysfs, "", false);
    }

    {
        // A machine-wide pressure reading would return the exhausted card here. Sizing a
        // launch against that number is what this probe exists to avoid.
        FakeSysfs sysfs("hybrid");
        sysfs.add_node("0", "0", "-1");
        sysfs.add_node("1", "110501", "128");
        sysfs.add_node("2", "110000", "129");
        sysfs.add_memory("128", "0", "34359738368");
        sysfs.add_memory("129", "17179869184", "17179869184");
        failures += !expect_device_memory(
            "an exhausted second GPU does not affect the requested one",
            sysfs, "gfx1151", true, 34359738368ULL, 34359738368ULL);
    }

    {
        FakeSysfs sysfs("duplicate");
        sysfs.add_node("1", "110501", "128");
        sysfs.add_node("2", "110501", "129");
        sysfs.add_memory("128", "0", "34359738368");
        sysfs.add_memory("129", "0", "34359738368");
        failures += !expect_device_memory(
            "two GPUs of one ISA are ambiguous and go unreported",
            sysfs, "gfx1151", false);
    }

    {
        FakeSysfs sysfs("norender");
        sysfs.add_node("1", "110501", "-1");
        failures += !expect_device_memory(
            "a GPU with no render node goes unreported",
            sysfs, "gfx1151", false);
    }

    {
        FakeSysfs sysfs("unreadable");
        sysfs.add_node("1", "110501", "128");
        failures += !expect_device_memory(
            "a missing memory reading goes unreported",
            sysfs, "gfx1151", false);
    }

    {
        FakeSysfs sysfs("zerototal");
        sysfs.add_node("1", "110501", "128");
        sysfs.add_memory("128", "0", "0");
        failures += !expect_device_memory(
            "a zero total goes unreported",
            sysfs, "gfx1151", false);
    }

    {
        FakeSysfs sysfs("overused");
        sysfs.add_node("1", "110501", "128");
        sysfs.add_memory("128", "34359738369", "34359738368");
        failures += !expect_device_memory(
            "used exceeding total goes unreported",
            sysfs, "gfx1151", false);
    }

    {
        FakeSysfs sysfs("empty");
        failures += !expect_device_memory(
            "an empty topology goes unreported",
            sysfs, "gfx1151", false);
    }

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
