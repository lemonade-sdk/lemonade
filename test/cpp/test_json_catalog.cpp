#include <lemon/models/json_catalog.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

using lemon::ModelInfo;
using lemon::json;
using lemon::models::CatalogAccumulator;
using lemon::models::CatalogDescriptor;
using lemon::models::find_registry_row;
using lemon::models::JsonCatalog;
using lemon::models::RegistryDocument;
using lemon::models::RegistryDocuments;
using lemon::models::RegistryRowRules;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static void check_eq(const std::string& name, const std::string& actual,
                     const std::string& expected) {
    const bool ok = actual == expected;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) {
        std::printf("       expected: %s\n       actual:   %s\n", expected.c_str(),
                    actual.c_str());
        ++g_failures;
    }
}

namespace {

struct Recorder {
    int paths = 0;
    int components = 0;
    int devices = 0;
    std::map<std::string, json> authored;
};

RegistryRowRules rules(Recorder& rec, const std::string& prefix = {}, bool suggested = false) {
    RegistryRowRules r;
    r.name_prefix = prefix;
    r.suggested_default = suggested;
    r.source_label = "test model";
    r.device_for_recipe = [&rec](const std::string&) {
        ++rec.devices;
        return lemon::DEVICE_CPU;
    };
    r.populate_components = [&rec](ModelInfo& info) {
        ++rec.components;
        info.components.push_back("FromManifest");
    };
    r.resolve_paths = [&rec](ModelInfo& info) {
        ++rec.paths;
        info.resolved_paths["main"] = "/tmp/" + info.model_name;
    };
    r.recipe_options = [&rec](const std::string& name, const json& authored) {
        rec.authored[name] = authored;
    };
    return r;
}

std::optional<ModelInfo> row(const json& body, const RegistryRowRules& r) {
    return lemon::models::parse_registry_row("M", body, r);
}

}  // namespace

static void test_the_name_prefix_is_minted_not_read() {
    Recorder rec;
    check_eq("a built-in row keeps its key", row(json::object(), rules(rec))->model_name, "M");
    check_eq("a registration row is minted under user.",
             row(json::object(), rules(rec, "user."))->model_name, "user.M");
}

static void test_checkpoint_grammar() {
    Recorder rec;
    const RegistryRowRules r = rules(rec);

    const ModelInfo plain = *row(json{{"checkpoint", "org/repo"}}, r);
    check_eq("checkpoint is the main reference", plain.checkpoint(), "org/repo");
    check("a row without mmproj has no mmproj checkpoint", plain.checkpoints.count("mmproj") == 0);

    const ModelInfo legacy =
        *row(json{{"checkpoint", "org/repo"}, {"mmproj", "mm.gguf"}}, r);
    check_eq("the legacy mmproj key becomes a variant of the repo",
             legacy.checkpoint("mmproj"), "org/repo:mm.gguf");

    const ModelInfo explicit_wins =
        *row(json{{"checkpoint", "org/repo"},
                  {"mmproj", "mm.gguf"},
                  {"checkpoints", {{"mmproj", "named.gguf"}}}},
             r);
    check_eq("an explicit checkpoint entry outranks the derived one",
             explicit_wins.checkpoint("mmproj"), "named.gguf");

    const ModelInfo named = *row(json{{"checkpoints", {{"main", "a"}, {"npu_cache", "b"}}}}, r);
    check("every named checkpoint survives",
          named.checkpoint() == "a" && named.checkpoint("npu_cache") == "b");
}

static void test_row_defaults_come_from_the_rules() {
    Recorder rec;

    check("suggested defaults to what the rules say", !row(json::object(), rules(rec))->suggested);
    check("a registration is suggested unless it says otherwise",
          row(json::object(), rules(rec, "user.", true))->suggested);
    check("an explicit suggested false survives that default",
          !row(json{{"suggested", false}}, rules(rec, "user.", true))->suggested);

    check("auto_update is only read when it is a bool",
          !row(json{{"auto_update", "true"}}, rules(rec))->auto_update.has_value());
    check("a bool auto_update is read",
          row(json{{"auto_update", true}}, rules(rec))->auto_update.value_or(false));
}

static void test_components() {
    Recorder rec;
    const ModelInfo info = *row(json{{"components", {"A", 3, "B"}}}, rules(rec));
    check("only string components are taken", info.components.size() == 2);
    check("in document order", info.components == std::vector<std::string>{"A", "B"});
}

static void test_a_bad_row_costs_one_model() {
    Recorder rec;
    const RegistryRowRules r = rules(rec);

    check("a row naming two different hosts is refused",
          !row(json{{"source", "huggingface"}, {"registry_source", "modelscope"}}, r));
    check("a row naming a host that is not registered is refused",
          !row(json{{"registry_source", "gitlfs"}}, r));

    const ModelInfo alias = *row(json{{"registry_source", "HF"}}, r);
    check_eq("a host is stored by its canonical name", alias.registry_source, "huggingface");
    check("a host is not a local origin", alias.source.empty());

    // One refused row must not take the document down with it.
    CatalogAccumulator acc;
    const json document = {
        {"Bad", {{"source", "huggingface"}, {"registry_source", "modelscope"}}},
        {"Malformed", {{"recipe", "llamacpp"}, {"checkpoints", {{"main", {{"key", "value"}}}}}}},
        {"Good", {{"recipe", "llamacpp"}}},
    };
    const JsonCatalog catalog(CatalogDescriptor{"user_models.json"},
                              std::make_shared<const json>(document), rules(rec, "user."));
    catalog.enumerate(acc);
    check("the readable rows of a broken document survive", acc.size() == 1);
    check("and one is keyed by its minted name", acc.contains("user.Good"));

    // A value of the wrong type throws inside the ingest, not out of the build.
    CatalogAccumulator typed;
    const json mistyped = {
        {"NotAString", {{"checkpoints", {{"main", json::object()}}}}},
        {"AlsoGood", {{"recipe", "llamacpp"}}},
    };
    const JsonCatalog typed_catalog(CatalogDescriptor{"user_models.json"},
                                   std::make_shared<const json>(mistyped), rules(rec, "user."));
    typed_catalog.enumerate(typed);
    check("a checkpoint of the wrong type costs its own row",
          typed.size() == 1 && typed.contains("user.AlsoGood"));
}

static void test_the_labels_hook_decides() {
    Recorder rec;
    RegistryRowRules refusing = rules(rec);
    refusing.stamp_labels = [](ModelInfo&) { return std::string("labels contradict the recipe"); };
    check("a row whose mode claims are illegal is refused",
          !row(json{{"labels", {"chat", "embeddings"}}}, refusing));

    RegistryRowRules stamping = rules(rec);
    stamping.stamp_labels = [](ModelInfo& info) {
        info.labels.push_back("embeddings");
        return std::string();
    };
    const ModelInfo stamped = *row(json::object(), stamping);
    check("a legal row is kept", stamped.model_name == "M");
    check("the stamp is visible to the caller",
          std::find(stamped.labels.begin(), stamped.labels.end(), "embeddings") !=
              stamped.labels.end());
    check("labels decide the model's type", stamped.type == lemon::ModelType::EMBEDDING);
}

static void test_hooks_run_once_per_row_and_never_sink_it() {
    Recorder rec;
    RegistryRowRules throwing_resolve = rules(rec);
    throwing_resolve.resolve_paths = [](ModelInfo&) { throw std::runtime_error("no such cache"); };

    const std::optional<ModelInfo> unresolved = row(json{{"checkpoint", "org/repo"}},
                                                   throwing_resolve);
    check("an unresolvable path leaves the model listed", unresolved.has_value());
    check("and not resolved", unresolved && unresolved->resolved_path().empty());

    Recorder counted;
    row(json{{"checkpoint", "org/repo"}}, rules(counted));
    check("the device question is asked once", counted.devices == 1);
    check("paths are resolved once", counted.paths == 1);
    check("a non-collection never asks for manifest components", counted.components == 0);

    Recorder inline_collection;
    const ModelInfo inline_only =
        *row(json{{"recipe", lemon::COLLECTION_OMNI_MODEL_RECIPE}, {"components", {"A"}}},
             rules(inline_collection));
    check("an inline collection with no repo pointer keeps what it authored",
          inline_only.components == std::vector<std::string>{"A"});
    check("so the manifest is not consulted", inline_collection.components == 0);

    Recorder pointered;
    const ModelInfo remote =
        *row(json{{"recipe", lemon::COLLECTION_OMNI_MODEL_RECIPE},
                  {"checkpoint", "org/manifest-repo"},
                  {"components", {"Stale"}}},
             rules(pointered));
    check("a collection with a repo pointer takes the cached manifest",
          remote.components == std::vector<std::string>{"FromManifest"});
}

static void test_the_authored_options_layer_is_reported() {
    Recorder rec;
    row(json{{"recipe_options", {{"ctx_size", 4096}}}}, rules(rec));
    check("the row's own options reach the caller", rec.authored.count("M") == 1);
    check_eq("and they are the row's values", rec.authored["M"].dump(),
             json{{"ctx_size", 4096}}.dump());

    Recorder untouched;
    const RegistryRowRules r = rules(untouched);
    row(json{{"recipe_options", "not an object"}}, r);
    check("a malformed options block is not reported", untouched.authored.empty());
    row(json{{"recipe", "llamacpp"}}, r);
    check("a row with no options block reports nothing", untouched.authored.empty());
}

static void test_unconsumed_keys_survive_as_extras() {
    Recorder rec;
    const ModelInfo info =
        *row(json{{"checkpoint", "org/repo"}, {"labels", {"chat"}}, {"sdcpp_args", "--foo"}},
             rules(rec));
    check("a key no typed field consumed is kept for the backend",
          info.extras.count("sdcpp_args") == 1);
    check("a key a typed field consumed is not duplicated there",
          info.extras.count("checkpoint") == 0 && info.extras.count("labels") == 0);
}

static lemon::models::RegistryDocuments::Pin pin() {
    lemon::models::RegistryDocuments::Pin documents;
    documents.builtin = std::make_shared<const json>(
        json{{"Foo", json{{"recipe", "llamacpp"}}}, {"Bar", json{{"recipe", "llamacpp"}}}});
    documents.user = std::make_shared<const json>(
        json{{"Bar", json{{"recipe", "llamacpp"}}}, {"Mine", json{{"recipe", "llamacpp"}}}});
    return documents;
}

static void test_a_requested_name_finds_its_row() {
    const lemon::models::RegistryDocuments::Pin documents = pin();

    const auto bare_builtin = find_registry_row(documents, "Foo");
    check("a bare name finds the built-in row",
          bare_builtin && bare_builtin->document == RegistryDocument::Builtin &&
              bare_builtin->key == "Foo" && bare_builtin->canonical_id == "Foo");

    const auto registered = find_registry_row(documents, "user.Mine");
    check("a user handle finds the user row by its bare part",
          registered && registered->document == RegistryDocument::User &&
              registered->key == "Mine" && registered->canonical_id == "user.Mine");

    const auto prefixed_builtin = find_registry_row(documents, "builtin.Bar");
    check("a built-in handle finds the row the registry keys bare",
          prefixed_builtin && prefixed_builtin->document == RegistryDocument::Builtin &&
              prefixed_builtin->key == "Bar");

    check("a scanned handle names bytes, not a row",
          !find_registry_row(documents, "extra.Foo").has_value());
    check("a name no document holds finds no row",
          !find_registry_row(documents, "Nothing").has_value());
    check("a user handle whose row is gone finds no row",
          !find_registry_row(documents, "user.Gone").has_value());
}

int main() {
    test_the_name_prefix_is_minted_not_read();
    test_checkpoint_grammar();
    test_row_defaults_come_from_the_rules();
    test_components();
    test_a_bad_row_costs_one_model();
    test_the_labels_hook_decides();
    test_hooks_run_once_per_row_and_never_sink_it();
    test_the_authored_options_layer_is_reported();
    test_unconsumed_keys_survive_as_extras();
    test_a_requested_name_finds_its_row();

    if (g_failures == 0) {
        std::printf("All JSON catalog tests passed.\n");
    } else {
        std::printf("%d JSON catalog test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
