// Unit tests for lemon::backends::BackendUtils::rocm_cache_dir_budget().
//
// The budget is the cliff between a ROCm install that works and one where
// rocBLAS access-violates partway through inference, so the arithmetic is
// pinned here against paths measured on real gfx1151 installs. If an upstream
// layout bump moves the deepest file, these numbers have to move with it.

#include <iostream>
#include <string>

#include <lemon/backends/backend_utils.h>

using lemon::backends::BackendUtils;

namespace {

int g_failures = 0;

void expect(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "[ok] " << what << std::endl;
    } else {
        std::cerr << "[FAIL] " << what << std::endl;
        ++g_failures;
    }
}

void expect_budget(const std::string& arch, const std::string& version, bool wheel, size_t want) {
    const size_t got = BackendUtils::rocm_cache_dir_budget(arch, version, wheel);
    const char* layout = wheel ? "wheel" : "tarball";
    if (got == want) {
        std::cout << "[ok] " << arch << "/" << version << " " << layout << " -> " << got
                  << std::endl;
    } else {
        std::cerr << "[FAIL] " << arch << "/" << version << " " << layout << " -> " << got
                  << " (wanted " << want << ")" << std::endl;
        ++g_failures;
    }
}

// Rebuild the deepest rocBLAS path from literal segments and check the budget against it.
void expect_models_path(const std::string& arch, const std::string& version, bool wheel) {
    const std::string tensile_name =
        "TensileLibrary_Type_4xi8I_HPA_Contraction_l_Ailk_Bjlk_Cijk_Dijk_fallback_" + arch +
        ".hsaco";
    const std::string layout =
        wheel ? "\\bin\\therock-wheels\\" + arch + "-" + version +
                    "\\venv\\Lib\\site-packages\\_rocm_sdk_libraries"
              : "\\bin\\therock\\" + arch + "-" + version;
    const std::string deepest = layout + "\\bin\\rocblas\\library\\" + arch + "\\" + tensile_name;

    const size_t want = 259 - deepest.size();
    const size_t got = BackendUtils::rocm_cache_dir_budget(arch, version, wheel);
    const char* name = wheel ? "wheel" : "tarball";
    if (got == want) {
        std::cout << "[ok] " << name << " budget " << got << " matches a " << deepest.size()
                  << "-char modelled path" << std::endl;
    } else {
        std::cerr << "[FAIL] " << name << " budget is " << got << " but the modelled path is "
                  << deepest.size() << " chars, implying " << want << std::endl;
        ++g_failures;
    }
}

}  // namespace

int main() {
    // The constants must model these exact paths, not merely sum to the right number.
    // Both measured on real gfx1151 installs.
    expect_models_path("gfx1151", "10.0.0", true);
    expect_models_path("gfx1151", "10.0.0", false);

    // The shipped pin.
    expect_budget("gfx1151", "10.0.0", true, 67);
    expect_budget("gfx1151", "10.0.0", false, 117);

    // Wheel budget is 67.
    expect(BackendUtils::rocm_cache_dir_budget("gfx1151", "10.0.0", true) >= 62,
           "62-char root is inside the wheel budget");
    expect(BackendUtils::rocm_cache_dir_budget("gfx1151", "10.0.0", true) < 73,
           "73-char root is outside the wheel budget");

    // The arch token appears three times in the deepest path (install dir,
    // library subdir, Tensile filename suffix), so one extra char costs three.
    expect_budget("gfx11511", "10.0.0", true, 64);

    // The version appears once.
    expect_budget("gfx1151", "10.0.0-rc1", true, 63);

    // The wheel layout is exactly 50 chars deeper than the tarball, for every
    // arch and version: the extra venv/site-packages nesting is fixed-width.
    for (const auto& arch : {std::string("gfx1151"), std::string("gfx942")}) {
        for (const auto& ver : {std::string("10.0.0"), std::string("7.14.0")}) {
            expect(BackendUtils::rocm_cache_dir_budget(arch, ver, false) -
                       BackendUtils::rocm_cache_dir_budget(arch, ver, true) ==
                   50,
                   "tarball budget exceeds wheel budget by exactly 50 for " + arch + "/" + ver);
        }
    }

    // Saturate rather than underflow when a layout eats the whole limit.
    expect(BackendUtils::rocm_cache_dir_budget(std::string(200, 'x'), "10.0.0", true) == 0,
           "absurd arch yields a zero budget, not an underflow");

    if (g_failures > 0) {
        std::cerr << g_failures << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "All ROCm path budget tests passed" << std::endl;
    return 0;
}
