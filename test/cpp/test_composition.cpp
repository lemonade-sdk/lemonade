// What a compose does with the rows its catalogs hand it: how options layer, what a row
// says about updating itself, and who is left standing on this machine. The catalogs are
// built here, so nothing reads a disk or a network.

#include <lemon/model_manager.h>
#include <lemon/model_types.h>
#include <lemon/models/artifact_source.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/catalog.h>
#include <lemon/models/catalog_set.h>
#include <lemon/models/composition.h>
#include <lemon/models/json_catalog.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using lemon::ModelInfo;
using lemon::json;
using lemon::models::ArtifactSource;
using lemon::models::AvailabilityProbe;
using lemon::models::BackendAvailability;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogSet;
using lemon::models::Conflict;
using lemon::models::CompositionResult;
using lemon::models::DocEntry;
using lemon::models::EnumeratedCatalog;
using lemon::models::JsonCatalog;
using lemon::models::make_catalog_descriptor;
using lemon::models::ModelArtifactStore;
using lemon::models::ModelComposition;
using lemon::models::RecipeAvailability;
using lemon::models::RegistryRowRules;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

// A machine that runs everything, so a check blames the compose rather than the probe.
static AvailabilityProbe runs_everything() {
    return [](const std::map<std::string, ModelInfo>&) {
        BackendAvailability machine;
        machine.filtering_disabled = true;
        return machine;
    };
}

// Rows the composition will not go looking for bytes over: each names a checkpoint that
// is nowhere on this machine.
static json registry_row(const std::string& name, const json& extra = json::object()) {
    json entry{{"recipe", "llamacpp"}, {"labels", {"chat"}}, {"size", 0.5},
               {"checkpoint", "nowhere/" + name + ":F16"}};
    for (const auto& [key, value] : extra.items()) {
        entry[key] = value;
    }
    return entry;
}

struct Harness {
    json registry = json::object();
    std::vector<ModelInfo> enumerated;
    json saved = json::object();
    AvailabilityProbe machine = runs_everything();
    std::filesystem::path cache_dir = fs::temp_directory_path() / "composition_test_cache";

    ModelArtifactStore artifacts{ModelArtifactStore::Hooks{
        [this] { return cache_dir.string(); },
        [](const ModelInfo&, const std::string&) -> std::unique_ptr<ArtifactSource> {
            return nullptr;
        },
        [] { return std::string(); },
    }};

    CompositionResult run() {
        ModelComposition::Deps deps;
        deps.load_catalogs = [this](std::map<std::string, json>& authored, CatalogSet& set) {
            // A registry row's own options reach a compose through this rule, which is
            // how the manager wires it too.
            RegistryRowRules rules;
            rules.recipe_options = [&authored](const std::string& name, const json& value) {
                authored[name] = value;
            };
            set.add(std::make_shared<JsonCatalog>(
                make_catalog_descriptor("test_registry", 0, Conflict::Overwrite,
                                        DocEntry{"test", "test", "test", {}, ""}),
                std::make_shared<const json>(registry), rules));
            if (!enumerated.empty()) {
                set.add(std::make_shared<EnumeratedCatalog>(
                    make_catalog_descriptor("test_scan", 1, Conflict::KeepExisting,
                                            DocEntry{"scan", "scan", "scan", {}, ""}),
                    [this](const CatalogAccumulator&) { return enumerated; }));
            }
        };
        deps.saved_options = [this] { return saved; };
        deps.availability = machine;
        deps.fetch = nullptr;
        deps.artifacts = &artifacts;
        return ModelComposition(std::move(deps)).run();
    }
};

static void test_rows_and_catalogs_travel() {
    Harness h;
    h.registry = json{{"Alpha", registry_row("Alpha")}, {"Beta", registry_row("Beta")}};
    CompositionResult composed = h.run();

    check("every row a catalog listed is in the set",
          composed.models.count("Alpha") == 1 && composed.models.count("Beta") == 1);
    check("and the catalogs that produced it come along, so the store can say who listed what",
          composed.catalogs.size() == 1);
    check("bytes that are not here leave the set counting nothing downloaded",
          composed.downloaded == 0);
}

static void test_options_layer_lowest_first() {
    Harness h;
    h.registry = json{{"Authored", registry_row("Authored", {{"recipe_options",
                                                              {{"ctx_size", 2048}}}})},
                      {"Saved", registry_row("Saved", {{"recipe_options",
                                                        {{"ctx_size", 2048}}}})}};
    h.saved = json{{"builtin.Saved", {{"ctx_size", 4096}}}};

    CompositionResult composed = h.run();
    check("what a row authored reaches the row",
          composed.models["Authored"].recipe_options.get_explicit_option("ctx_size") == 2048);
    check("and what the user saved outranks it",
          composed.models["Saved"].recipe_options.get_explicit_option("ctx_size") == 4096);
}

static void test_a_row_says_whether_it_updates_itself() {
    Harness h;
    h.registry = json{{"KeepsCurrent", registry_row("KeepsCurrent", {{"recipe_options",
                                                                      {{"auto_update", true}}}})},
                      {"HoldsStill", registry_row("HoldsStill", {{"recipe_options",
                                                                  {{"auto_update", false}}}})},
                      {"Overridden", registry_row("Overridden", {{"recipe_options",
                                                                  {{"auto_update", false}}}})},
                      {"Undecided", registry_row("Undecided")}};
    h.saved = json{{"builtin.Overridden", {{"auto_update", true}}}};

    CompositionResult composed = h.run();
    check("a row that asks to be kept current says so",
          composed.models["KeepsCurrent"].auto_update == std::optional<bool>(true));
    check("a row that asks out of it says so",
          composed.models["HoldsStill"].auto_update == std::optional<bool>(false));
    check("and what the user saved about updating outranks the row",
          composed.models["Overridden"].auto_update == std::optional<bool>(true));
    check("a row that says nothing leaves the machine default alone",
          !composed.models["Undecided"].auto_update.has_value());
}

static void test_this_machine_decides_who_is_left() {
    Harness h;
    h.registry = json{{"Alpha", registry_row("Alpha")}};
    h.machine = [](const std::map<std::string, ModelInfo>&) {
        BackendAvailability machine;
        machine.recipes["llamacpp"] = RecipeAvailability{"no backend here", "", false};
        return machine;
    };

    CompositionResult composed = h.run();
    check("a recipe this machine cannot run leaves the set", composed.models.empty());
    check("and the reason is kept for whoever asks why",
          composed.reasons && composed.reasons->rejected.size() == 1);
}

static void test_a_row_without_a_label_is_still_a_row() {
    Harness h;
    ModelInfo unlabelled;
    unlabelled.model_name = "Unlabelled";
    unlabelled.recipe = "llamacpp";
    unlabelled.size = 0.5;
    h.enumerated = {unlabelled};

    check("a missing deployment label is a warning, not a filter",
          h.run().models.count("Unlabelled") == 1);
}

int main() {
    test_rows_and_catalogs_travel();
    test_options_layer_lowest_first();
    test_a_row_says_whether_it_updates_itself();
    test_this_machine_decides_who_is_left();
    test_a_row_without_a_label_is_still_a_row();

    if (g_failures != 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll composition checks passed\n");
    return 0;
}
