// The machine facts the hardware filter is built from: which memory a reported
// device can offer a model, and what the probe answers for the rest.

#include "lemon/config_file.h"
#include "lemon/model_manager.h"
#include "lemon/models/host_availability.h"
#include "lemon/runtime_config.h"

#include <map>
#include <string>

using lemon::ConfigFile;
using lemon::ModelInfo;
using lemon::RuntimeConfig;
using lemon::json;
using lemon::models::BackendAvailability;
using lemon::models::device_memory_pool_gb;
using lemon::models::probe_backend_availability;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static json device(double vram_gb, double virtual_mem_gb, bool integrated) {
    json dev;
    dev["vram_gb"] = vram_gb;
    dev["virtual_mem_gb"] = virtual_mem_gb;
    dev["integrated"] = integrated;
    return dev;
}

static void test_which_memory_counts() {
    check("a discrete GPU offers its VRAM, not its aperture",
          device_memory_pool_gb("amd_gpu", device(24.0, 96.0, false), false) == 24.0);

    // An APU is reported under "amd_gpu" and runs models out of system RAM.
    check("an integrated GPU offers its aperture",
          device_memory_pool_gb("amd_gpu", device(0.5, 24.0, true), false) == 24.0);

    check("a device that does not say it is discrete counts as integrated",
          device_memory_pool_gb("amd_gpu", json{{"vram_gb", 0.5}, {"virtual_mem_gb", 24.0}},
                                false) == 24.0);

    check("the GTT opt-in offers aperture and VRAM together",
          device_memory_pool_gb("amd_gpu", device(24.0, 96.0, false), true) == 120.0);

    check("a device of another make keeps its own aperture private",
          device_memory_pool_gb("nvidia_gpu", device(8.0, 64.0, true), false) == 8.0);

    check("a device that reports no memory offers none",
          device_memory_pool_gb("amd_gpu", json::object(), true) == 0.0);
}

static ModelInfo model(const std::string& recipe) {
    ModelInfo info;
    info.recipe = recipe;
    return info;
}

static void test_the_probe_answers() {
    std::map<std::string, ModelInfo> models = {
        {"A", model("llamacpp")},
        {"B", model("llamacpp")},
        {"C", model("ds4")},
        {"D", model("cloud")},
    };

    RuntimeConfig unfiltered(json{{"disable_model_filtering", true}});
    RuntimeConfig::set_global(&unfiltered);
    const BackendAvailability off = probe_backend_availability(models);
    check("filtering off answers without asking about a recipe",
          off.filtering_disabled && off.recipes.empty());

    json full = lemon::ConfigFile::base_defaults();
    full["enable_dgpu_gtt"] = true;
    RuntimeConfig filtered(full);
    RuntimeConfig::set_global(&filtered);
    const BackendAvailability on = probe_backend_availability(models);
    RuntimeConfig::set_global(nullptr);

    check("the GTT opt-in reaches the filter", on.assume_dgpu_gtt);
    check("a recipe is asked about once, however many models name it",
          on.recipes.count("llamacpp") == 1 && on.recipes.size() == 3);
    check("a backend that reads its model from disk says so",
          on.recipe("ds4").streams_model_from_storage);
    check("a recipe with no hardware gating of its own is never held back",
          on.recipe("cloud").unsupported_reason.empty());

    const BackendAvailability unknown =
        probe_backend_availability({{"X", model("no-such-recipe")}});
    check("a recipe nothing recognizes is held back, with a reason",
          !unknown.recipe("no-such-recipe").unsupported_reason.empty());
}

int main() {
    test_which_memory_counts();
    test_the_probe_answers();

    if (g_failures == 0) {
        std::printf("All host availability tests passed.\n");
    } else {
        std::printf("%d host availability test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
