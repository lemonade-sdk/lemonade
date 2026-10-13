#include "lemon/model_manager.h"
#include "lemon/models/store_builder.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

using lemon::ModelInfo;
using lemon::models::BackendAvailability;
using lemon::models::FilterResult;
using lemon::models::RejectCause;
using lemon::models::RecipeAvailability;
using lemon::models::filter_hardware;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static ModelInfo model(const std::string& recipe, double size_gb) {
    ModelInfo info;
    info.recipe = recipe;
    info.size = size_gb;
    return info;
}

// A machine with 32 GB RAM and a 20 GB largest pool, where every recipe is
// supported and nothing is missing.
static BackendAvailability supported_machine() {
    BackendAvailability availability;
    availability.system_ram_gb = 32.0;
    availability.largest_mem_pool_gb = 20.0;
    availability.processor = "Test CPU";
    availability.os_version = "Test OS";
    return availability;
}

static const char* reason_for(const FilterResult& result, const std::string& id) {
    for (const auto& rejected : result.rejected) {
        if (rejected.canonical_id == id) return rejected.reason.c_str();
    }
    return nullptr;
}

static const RejectCause* cause_for(const FilterResult& result, const std::string& id) {
    for (const auto& rejected : result.rejected) {
        if (rejected.canonical_id == id) return &rejected.cause;
    }
    return nullptr;
}

static void test_disabled_filtering_passes_everything() {
    BackendAvailability availability = supported_machine();
    availability.filtering_disabled = true;
    // Unsupported and enormous, yet nothing may be hidden.
    availability.recipes["llamacpp"] = RecipeAvailability{"no backend here", "", false};

    std::map<std::string, ModelInfo> models;
    models["Huge"] = model("llamacpp", 5000.0);
    models["Tiny"] = model("llamacpp", 1.0);

    const FilterResult result = filter_hardware(models, availability);
    check("disabled filtering keeps every model", result.visible.size() == 2);
    check("disabled filtering records no rejection", result.rejected.empty());
    check("disabled filtering records no empty recipe", result.empty_recipes.empty());
}

static void test_collections_and_cloud_are_always_visible() {
    BackendAvailability availability = supported_machine();
    availability.recipes["collection.omni"] = RecipeAvailability{"missing", "", false};
    availability.recipes["cloud"] = RecipeAvailability{"missing", "", false};

    std::map<std::string, ModelInfo> models;
    models["Omni"] = model("collection.omni", 9000.0);
    models["Routed"] = model("collection.router", 9000.0);
    models["Remote"] = model("cloud", 9000.0);

    const FilterResult result = filter_hardware(models, availability);
    check("a collection stays visible despite an unsupported recipe",
          result.visible.count("Omni") == 1);
    check("a router collection stays visible", result.visible.count("Routed") == 1);
    check("a cloud model bypasses local machine checks", result.visible.count("Remote") == 1);
    check("nothing invisible was recorded", result.rejected.empty());
}

static void test_backend_missing_rejections() {
    BackendAvailability availability = supported_machine();
    availability.recipes["ds4"] = RecipeAvailability{"ds4 is not supported", "", false};
    availability.recipes["whispercpp"] = RecipeAvailability{"", "whisper backend not installed",
                                                            false};

    std::map<std::string, ModelInfo> models;
    models["Unsupported"] = model("ds4", 1.0);
    models["GuestMissing"] = model("whispercpp", 1.0);
    models["Fine"] = model("llamacpp", 1.0);

    const FilterResult result = filter_hardware(models, availability);
    check("an unsupported recipe rejects its model", result.visible.count("Unsupported") == 0);
    check("a missing guest backend rejects its model",
          result.visible.count("GuestMissing") == 0);
    check("a healthy recipe keeps its model", result.visible.count("Fine") == 1);
    check("two models rejected", result.rejected.size() == 2);

    const char* unsupported_reason = reason_for(result, "Unsupported");
    check("the unsupported reason is reported", unsupported_reason != nullptr);
    if (unsupported_reason) {
        check("the reason names the machine processor",
              std::string(unsupported_reason).find("Test CPU") != std::string::npos);
        check("the reason names the operating system",
              std::string(unsupported_reason).find("Test OS") != std::string::npos);
    }
    check("an unsupported recipe is a backend-missing cause",
          cause_for(result, "Unsupported") &&
              *cause_for(result, "Unsupported") == RejectCause::BackendMissing);
    check("a missing guest backend is a backend-missing cause",
          cause_for(result, "GuestMissing") &&
              *cause_for(result, "GuestMissing") == RejectCause::BackendMissing);
}

static void test_memory_fit_rules() {
    BackendAvailability availability = supported_machine();
    // Threshold is max(largest pool, 80% of RAM) = 25.6 GB.
    std::map<std::string, ModelInfo> models;
    models["JustUnder"] = model("llamacpp", 25.0);
    models["JustOver"] = model("llamacpp", 26.0);

    const FilterResult result = filter_hardware(models, availability);
    check("a model within the threshold stays visible",
          result.visible.count("JustUnder") == 1);
    check("a model over the threshold is hidden", result.visible.count("JustOver") == 0);
    check("an over-threshold model is a memory-fit cause",
          cause_for(result, "JustOver") && *cause_for(result, "JustOver") == RejectCause::MemoryFit);

    // A zero system RAM reading means the machine is unknown: hide nothing.
    BackendAvailability unknown_ram = supported_machine();
    unknown_ram.system_ram_gb = 0.0;
    unknown_ram.largest_mem_pool_gb = 0.0;
    std::map<std::string, ModelInfo> one;
    one["Huge"] = model("llamacpp", 900.0);
    check("an unknown machine hides nothing",
          filter_hardware(one, unknown_ram).visible.count("Huge") == 1);
}

static void test_streaming_models_filter_on_working_set() {
    BackendAvailability availability = supported_machine();
    RecipeAvailability streaming;
    streaming.streams_model_from_storage = true;
    availability.recipes["llamacpp"] = streaming;

    // 81 GB on disk but only 16 GB resident: fits a 20 GB pool.
    ModelInfo streams_small = model("llamacpp", 81.0);
    streams_small.min_resident_gb = 16.0;
    // No declared resident footprint, so the whole model must fit the pool.
    ModelInfo streams_all = model("llamacpp", 81.0);

    std::map<std::string, ModelInfo> models;
    models["SmallResident"] = streams_small;
    models["FullResident"] = streams_all;

    const FilterResult result = filter_hardware(models, availability);
    check("a streaming model fits on its working set",
          result.visible.count("SmallResident") == 1);
    check("a streaming model without a declared footprint uses its full size",
          result.visible.count("FullResident") == 0);
    check("the streaming rejection is a memory-fit cause",
          cause_for(result, "FullResident") &&
              *cause_for(result, "FullResident") == RejectCause::MemoryFit);
}

static void test_locally_owned_models_are_exempt() {
    BackendAvailability availability = supported_machine();
    std::map<std::string, ModelInfo> models;
    models["user.Huge"] = model("llamacpp", 900.0);
    models["extra.Huge"] = model("llamacpp", 900.0);

    ModelInfo uploaded = model("llamacpp", 900.0);
    uploaded.source = "local_upload";
    models["Uploaded"] = uploaded;

    const FilterResult result = filter_hardware(models, availability);
    check("a registered model is never hidden by machine fit",
          result.visible.count("user.Huge") == 1);
    check("a discovered model is never hidden by machine fit",
          result.visible.count("extra.Huge") == 1);
    check("an uploaded model is never hidden by machine fit",
          result.visible.count("Uploaded") == 1);
    check("no exemption still records a rejection", result.rejected.empty());
}

static void test_empty_recipes() {
    BackendAvailability availability = supported_machine();
    std::map<std::string, ModelInfo> models;
    models["Gone1"] = model("ds4", 900.0);
    models["Gone2"] = model("ds4", 800.0);
    models["Stays"] = model("llamacpp", 1.0);

    const FilterResult result = filter_hardware(models, availability);
    check("a recipe with every model hidden is reported",
          std::find(result.empty_recipes.begin(), result.empty_recipes.end(), "ds4") !=
              result.empty_recipes.end());
    check("a recipe with a visible model is not reported",
          std::find(result.empty_recipes.begin(), result.empty_recipes.end(), "llamacpp") ==
              result.empty_recipes.end());
}

static void test_backend_missing_does_not_report_empty_recipe() {
    BackendAvailability availability = supported_machine();
    availability.recipes["ds4"] = RecipeAvailability{"not supported", "", false};

    std::map<std::string, ModelInfo> models;
    models["Unsupported"] = model("ds4", 1.0);

    const FilterResult result = filter_hardware(models, availability);
    check("a rejected model is recorded", result.rejected.size() == 1);
    // Only memory fit feeds the recipe availability table, as before.
    check("a backend-missing rejection leaves the recipe table alone",
          result.empty_recipes.empty());
}

static void test_filter_is_pure() {
    BackendAvailability availability = supported_machine();
    std::map<std::string, ModelInfo> models;
    models["Big"] = model("llamacpp", 900.0);
    models["Small"] = model("llamacpp", 2.0);

    const FilterResult first = filter_hardware(models, availability);
    const FilterResult second = filter_hardware(models, availability);
    check("the same inputs produce the same visible set", first.visible.size() == second.visible.size());
    check("the same inputs produce the same rejections", first.rejected.size() == second.rejected.size());
    check("the input map is not modified by filtering", models.size() == 2);
}

int main() {
    test_disabled_filtering_passes_everything();
    test_collections_and_cloud_are_always_visible();
    test_backend_missing_rejections();
    test_memory_fit_rules();
    test_streaming_models_filter_on_working_set();
    test_locally_owned_models_are_exempt();
    test_empty_recipes();
    test_backend_missing_does_not_report_empty_recipe();
    test_filter_is_pure();

    if (g_failures == 0) {
        std::printf("All hardware filter tests passed.\n");
    } else {
        std::printf("%d hardware filter test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
