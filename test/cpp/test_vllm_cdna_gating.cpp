#include <algorithm>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "lemon/backends/vllm/vllm_arg_resolver.h"
#include "lemon/system_info.h"

using lemon::SystemInfo;
using lemon::backends::device_class_launch_policy;
using lemon::backends::is_discrete_hbm_arch;
using lemon::backends::resolve_vllm_args;

namespace {

int failures = 0;

void expect(bool condition, const std::string& label) {
    if (condition) {
        std::cout << "PASS: " << label << std::endl;
    } else {
        std::cout << "FAIL: " << label << std::endl;
        ++failures;
    }
}

}  // namespace

int main() {
    // gfx942 is not in the installable matrix until its asset ships, though everything
    // below it is wired and tested.
    expect(!SystemInfo::backend_supports_arch("vllm", "rocm", "gfx942"),
           "vllm:rocm gfx942 is NOT advertised installable yet (asset pending; infra staged)");

    expect(SystemInfo::backend_supports_arch("vllm", "rocm", "gfx1100"),
           "vllm:rocm still supports gfx1100 via gfx110X wildcard");
    expect(SystemInfo::backend_supports_arch("vllm", "rocm", "gfx1151"),
           "vllm:rocm still supports gfx1151 (Strix Halo)");

    expect(!SystemInfo::backend_supports_arch("vllm", "rocm", "gfx906"),
           "vllm:rocm does not support gfx906 (not published)");
    expect(!SystemInfo::backend_supports_arch("vllm", "rocm", "gfx1036"),
           "vllm:rocm does not support gfx1036 (iGPU, not published)");

    expect(is_discrete_hbm_arch("gfx942"), "gfx942 is discrete-HBM class");
    expect(is_discrete_hbm_arch("gfx950"), "gfx950 is discrete-HBM class");
    expect(is_discrete_hbm_arch("gfx90a"), "gfx90a is discrete-HBM class");
    expect(!is_discrete_hbm_arch("gfx1151"), "gfx1151 (Strix Halo APU) is not discrete-HBM");
    expect(!is_discrete_hbm_arch("gfx1100"), "gfx1100 (consumer dGPU) keeps conservative defaults");
    expect(!is_discrete_hbm_arch("gfx1201"), "gfx1201 (RDNA4) keeps conservative defaults");
    expect(!is_discrete_hbm_arch(""), "empty arch is not discrete-HBM");

    expect(SystemInfo::rocm_asset_family("gfx942") == "gfx942",
           "rocm_asset_family passes gfx942 through unchanged (release tag arch)");
    expect(SystemInfo::rocm_asset_family("gfx1100") == "gfx110X",
           "rocm_asset_family collapses gfx1100 to gfx110X");

    // Per-arch version override: gfx942 (CDNA-dcgpu) pins a distinct vLLM/ROCm
    // release line from the RDNA default, since no single tag carries both.
    expect(SystemInfo::vllm_rocm_version_override("gfx942") == "vllm0.19.1-rocm7.13.0",
           "vllm gfx942 overrides to its own dcgpu release line");
    expect(SystemInfo::vllm_rocm_version_override("gfx110X").empty(),
           "vllm RDNA families use the default pin (no override)");
    expect(SystemInfo::vllm_rocm_version_override("gfx1151").empty(),
           "vllm gfx1151 uses the default pin (no override)");

    // The discrete-HBM vs conservative-default wiring that VLLMServer::load() applies.
    auto apu = device_class_launch_policy("gfx1151", false);
    expect(apu.enforce_eager && apu.cap_kv_cache && apu.force_awq_kernel,
           "gfx1151 (Strix Halo APU) keeps conservative defaults: eager + kv-cap + awq-force");
    auto cdna = device_class_launch_policy("gfx942", false);
    expect(!cdna.enforce_eager && !cdna.cap_kv_cache && !cdna.force_awq_kernel,
           "gfx942 (MI300X) gets vLLM-native budgeting: no eager, no kv-cap, no awq-force");
    auto cdna_budget = device_class_launch_policy("gfx942", true);
    expect(!cdna_budget.cap_kv_cache,
           "explicit user memory budget suppresses the kv-cap on discrete-HBM");
    auto rdna_dgpu = device_class_launch_policy("gfx1100", false);
    expect(rdna_dgpu.enforce_eager && rdna_dgpu.cap_kv_cache && rdna_dgpu.force_awq_kernel,
           "gfx1100 (consumer dGPU) keeps conservative defaults");

    // An explicit --enforce-eager forces eager even on discrete-HBM, leaving the other
    // discrete-HBM defaults intact.
    auto cdna_eager = device_class_launch_policy("gfx942", false, /*has_enforce_eager=*/true);
    expect(cdna_eager.enforce_eager,
           "explicit --enforce-eager overrides the discrete-HBM graph default on gfx942");
    expect(!cdna_eager.cap_kv_cache && !cdna_eager.force_awq_kernel,
           "the eager escape hatch does not disturb the other gfx942 defaults");

    // Reachability: the policy above only matters if --enforce-eager survives the
    // resolver — not rejected as protected, reported as intent, stripped from passthrough.
    {
        auto eager = resolve_vllm_args("m", "some/checkpoint", nlohmann::json::object(), "--enforce-eager");
        expect(eager.has_enforce_eager,
               "resolve_vllm_args accepts --enforce-eager and records it as a managed intent");
        bool leaked = std::find(eager.args.begin(), eager.args.end(), "--enforce-eager") != eager.args.end();
        expect(!leaked,
               "resolve_vllm_args strips --enforce-eager from passthrough args (no duplicate flag)");
    }
    {
        auto plain = resolve_vllm_args("m", "some/checkpoint", nlohmann::json::object(), "");
        expect(!plain.has_enforce_eager,
               "resolve_vllm_args without --enforce-eager reports has_enforce_eager=false");
    }

    // speculative_config (e.g. MTP) is read as an object and serialized for the backend.
    {
        nlohmann::json cfg;
        cfg["models"]["M"]["speculative_config"] = {{"method", "mtp"}, {"num_speculative_tokens", 1}};
        auto res = resolve_vllm_args("M", "cp", cfg, "");
        expect(!res.speculative_config.empty(),
               "resolve_vllm_args surfaces model speculative_config");
        expect(res.speculative_config.find("mtp") != std::string::npos &&
               res.speculative_config.find("num_speculative_tokens") != std::string::npos,
               "speculative_config serializes the MTP JSON for --speculative-config");
        auto none = resolve_vllm_args("M", "cp", nlohmann::json::object(), "");
        expect(none.speculative_config.empty(),
               "speculative_config is empty when the model config does not set it");
    }

    // speculative_config resolves family-first then model (model wins).
    {
        nlohmann::json cfg;
        cfg["families"]["fam"]["speculative_config"] = {{"method", "mtp"}, {"num_speculative_tokens", 2}};
        cfg["models"]["FM"]["family"] = "fam";
        auto fam = resolve_vllm_args("FM", "cp", cfg, "");
        expect(fam.speculative_config.find("\"num_speculative_tokens\":2") != std::string::npos,
               "family-level speculative_config is applied when the model sets none");
        cfg["models"]["FM"]["speculative_config"] = {{"method", "mtp"}, {"num_speculative_tokens", 1}};
        auto model_wins = resolve_vllm_args("FM", "cp", cfg, "");
        expect(model_wins.speculative_config.find("\"num_speculative_tokens\":1") != std::string::npos,
               "model-level speculative_config overrides the family-level one");
    }

    // speculative_config must be a JSON object — a scalar/array is a config mistake,
    // rejected with a clear error rather than silently dumped (fl0rianr review).
    {
        nlohmann::json cfg;
        cfg["models"]["M"]["speculative_config"] = "mtp";  // wrong: a string, not an object
        bool threw = false;
        try {
            resolve_vllm_args("M", "cp", cfg, "");
        } catch (const std::runtime_error&) {
            threw = true;
        }
        expect(threw, "a non-object speculative_config is rejected with a clear error");
    }

    // Release tags: a bare base gains the -{arch} suffix, a matching pin is used verbatim,
    // and a cross-arch pin is rejected rather than installed against the wrong line.
    {
        auto release_tag = [](const std::string& v, const std::string& arch) -> std::string {
            static const std::regex re("-(gfx[0-9a-fA-FxX]+)$");
            std::smatch match;
            if (std::regex_search(v, match, re)) {
                if (match[1].str() != arch) {
                    throw std::runtime_error("vLLM ROCm pin '" + v + "' targets " +
                                             match[1].str() + " but this host is " + arch);
                }
                return v;
            }
            return v + "-" + arch;
        };
        expect(release_tag("vllm0.19.1-rocm7.13.0", "gfx942") == "vllm0.19.1-rocm7.13.0-gfx942",
               "a bare base version gets the -gfx942 target suffix");
        expect(release_tag("vllm0.19.1-rocm7.13.0-gfx942", "gfx942") == "vllm0.19.1-rocm7.13.0-gfx942",
               "a full -gfx942 pin matching the host is used verbatim (no ...-gfx942-gfx942 double suffix)");
        bool cross_arch_rejected = false;
        try {
            release_tag("vllm0.20.1-rocm7.12.0-gfx110X", "gfx942");
        } catch (const std::runtime_error&) {
            cross_arch_rejected = true;
        }
        expect(cross_arch_rejected,
               "a cross-arch pin (gfx110X on a gfx942 host) is REJECTED, not installed against the wrong arch");
    }

    // The device list is iGPU-first, so a hybrid host (gfx1151 APU + gfx942 dGPU) must
    // still resolve gfx942.
    {
        nlohmann::json hybrid = nlohmann::json::array();
        hybrid.push_back({{"name", "AMD Radeon 8060S"}, {"family", "gfx1151"}, {"integrated", true}, {"available", true}});
        hybrid.push_back({{"name", "AMD Instinct MI300X"}, {"family", "gfx942"}, {"integrated", false}, {"available", true}});
        expect(SystemInfo::select_rocm_arch(hybrid) == "gfx942",
               "hybrid iGPU(gfx1151)+dGPU(gfx942): the discrete MI300X wins over the iGPU");

        nlohmann::json igpu_only = nlohmann::json::array();
        igpu_only.push_back({{"name", "AMD Radeon 8060S"}, {"family", "gfx1151"}, {"integrated", true}, {"available", true}});
        expect(SystemInfo::select_rocm_arch(igpu_only) == "gfx1151",
               "iGPU-only (Strix Halo) still resolves its integrated arch");

        nlohmann::json dgpu_only = nlohmann::json::array();
        dgpu_only.push_back({{"name", "AMD Instinct MI300X"}, {"family", "gfx942"}, {"integrated", false}, {"available", true}});
        expect(SystemInfo::select_rocm_arch(dgpu_only) == "gfx942",
               "dGPU-only resolves the discrete arch");

        nlohmann::json unavailable_dgpu = nlohmann::json::array();
        unavailable_dgpu.push_back({{"name", "AMD Radeon 8060S"}, {"family", "gfx1151"}, {"integrated", true}, {"available", true}});
        unavailable_dgpu.push_back({{"name", "AMD Instinct MI300X"}, {"family", "gfx942"}, {"integrated", false}, {"available", false}});
        expect(SystemInfo::select_rocm_arch(unavailable_dgpu) == "gfx1151",
               "an unavailable discrete GPU falls back to the available iGPU");
    }

    // Status must resolve the SAME per-arch override install writes, or gfx942 reads
    // update_required forever (the RDNA pin cannot prefix-match the dcgpu base).
    {
        std::string family = SystemInfo::rocm_asset_family("gfx942");
        std::string override_base = SystemInfo::vllm_rocm_version_override(family);
        std::string installed = override_base + "-" + family;  // what get_install_params writes
        auto prefix_match = [](const std::string& inst, const std::string& exp) {
            return inst == exp || (exp.size() < inst.size() &&
                                   inst.compare(0, exp.size() + 1, exp + "-") == 0);
        };
        expect(!override_base.empty(), "gfx942 pins a per-arch expected override line for status");
        expect(prefix_match(installed, override_base),
               "gfx942 install tag matches its per-arch expected version (fix: status resolves the override)");
        expect(!prefix_match(installed, "vllm0.20.1-rocm7.12.0"),
               "gfx942 install tag does NOT match the default RDNA base (why per-arch status resolution is required)");
    }

    if (failures != 0) {
        std::cout << failures << " assertion(s) failed" << std::endl;
        return 1;
    }
    std::cout << "All vllm CDNA gating assertions passed" << std::endl;
    return 0;
}
