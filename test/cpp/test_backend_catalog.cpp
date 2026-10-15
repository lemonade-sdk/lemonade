#include "catalog_fixture.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/configured/backend_catalog.h"
#include "llm_configured_fixture.h"

#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace llm_oracle;
using namespace catalog_fixture;
namespace fs = std::filesystem;

namespace {
void rejected(const std::vector<DefinitionSource>& sources, const std::string& expected) {
    bool found = false;
    try { BackendCatalog candidate(sources); }
    catch (const DefinitionValidationError& error) {
        for (const auto& row : error.diagnostics) found |= row.code == expected;
    }
    check(found && !backend_catalog(), "atomic rejection: " + expected);
}
DefinitionSource source(const json& doc) { return {"fixture.json", doc.dump(), DefinitionOrigin::External}; }
void put(const fs::path& file, const std::string& bytes) { std::ofstream(file) << bytes; }
}

int main(int argc, char** argv) {
    const auto root = fs::temp_directory_path() / ("lemon-catalog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    utils::set_cache_dir(root.string());
    utils::set_config_dir(root.string());
    struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{root};
    if (argc > 1 && std::string(argv[1]) == "--late") {
        fs::create_directories(root / "backends.d");
        put(root / "backends.d" / "a.json", definition().dump());
        RecipeOptions::known_keys();
        bool rejected = false;
        try { bootstrap_backend_catalog(root.string()); } catch (const std::logic_error&) { rejected = true; }
        check(rejected && !backend_catalog(), "late bootstrap cannot lose external defaults");
        return failures ? 1 : 0;
    }
    check(discover_backend_definitions(root.string()).empty(), "missing directory means no external definitions");
    check(schema_digest("abc") == "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "independent SHA-256 known-answer vector");
    auto alpha = definition();
    auto beta = beta_definition();
    const auto original = source(alpha);
    const auto b = source(beta);
    BackendCatalog local({original, b});
    auto schema = local.export_schema();
    check(schema["recipes"].size() == 19 && schema["recipes"][17]["recipe"] == "catalog-alpha" && schema["recipes"][18]["recipe"] == "catalog-beta", "17 built-ins then two external identities");
    const auto alpha_row = local.export_schema("catalog-alpha")["recipes"][0];
    check(alpha_row["options"][0] == json{{"name", "alpha_batch"}, {"value_type", "integer"}, {"default_value", 2}, {"unset_values", {nullptr}}, {"minimum", 1}, {"maximum", 8}, {"help", "Batch size"}, {"group", "Alpha"}, {"ui", {{"label", "Batch size"}, {"control", "number"}, {"order", 10}}}}, "hand-authored public option contract");
    check(alpha_row["operations"] == json::array({{{"operation", "chat_completion"}, {"delivery", "buffered"}, {"model_modes", {"chat"}}, {"provenance", "implemented"}}}), "hand-authored executable operation projection");
    check(alpha_row["source"] == "external" && alpha_row["definition_id"] == "catalog-alpha" && alpha_row["support"] == json::array() && alpha_row["operations_complete"] == true, "external row fields");
    check(local.export_schema("absent")["recipes"].empty(), "unknown filter representation");
    check(local.validate(original.text)["valid"] == true && local.validate(b.text)["valid"] == true, "matching external ID and recipe may substitute privately");
    auto changed = alpha; changed["id"] = "unrelated";
    check(local.validate(changed.dump())["valid"] == false, "recipe without matching ID cannot substitute");
    rejected({original, {"broken.json", "{", DefinitionOrigin::External}}, "syntax_error");
    rejected({original, original}, "id_collision");
    changed = beta; changed["id"] = "catalog-alpha";
    rejected({original, source(changed)}, "id_collision");
    changed = beta; changed["recipe"] = "catalog-alpha";
    rejected({original, source(changed)}, "recipe_collision");
    changed = alpha; changed["recipe"] = "ds4";
    rejected({source(changed)}, "recipe_collision");
    changed = alpha; changed["metadata"]["config_section"] = "host";
    rejected({source(changed)}, "section_collision");
    changed = beta; changed["metadata"]["config_section"] = "catalog-alpha";
    rejected({original, source(changed)}, "section_collision");
    for (const auto* key : {"ctx_size", "llamacpp_backend", "model_name", "save_options", "dry_run", "messages", "port"}) {
        changed = alpha; changed["metadata"]["options"][0]["name"] = key;
        rejected({source(changed)}, "option_collision");
    }
    changed = beta; changed["metadata"]["options"][0]["name"] = "alpha_bias";
    rejected({original, source(changed)}, "option_collision");
    changed = beta; changed["install"].erase("default_versions");
    rejected({source(changed)}, "required");
    auto parsed = BackendDefinitionStore().validate(original);
    auto formatted = original; formatted.text = alpha.dump(4);
    check(parsed.definition->digest == BackendDefinitionStore().validate(formatted).definition->digest, "whitespace canonical digest");
    const auto ordered = nlohmann::ordered_json::parse(original.text);
    nlohmann::ordered_json reverse = nlohmann::ordered_json::object();
    for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) reverse[it.key()] = it.value();
    check(parsed.definition->digest == BackendDefinitionStore().validate({"", reverse.dump(), DefinitionOrigin::External}).definition->digest, "object key order canonical digest");
    changed = alpha; changed["metadata"]["options"][0]["default_value"] = 3;
    check(BackendDefinitionStore().validate(source(changed)).definition->digest != parsed.definition->digest, "changed source value changes digest");
    changed = alpha; std::swap(changed["metadata"]["options"][1], changed["metadata"]["options"][2]);
    check(BackendDefinitionStore().validate(source(changed)).definition->digest != parsed.definition->digest, "array order changes digest");
    check(BackendCatalog({original, b}).export_schema()["catalog_id"] == schema["catalog_id"], "identical catalog stable");
    check(BackendCatalog({b, original}).export_schema()["catalog_id"] != schema["catalog_id"], "catalog order participates in digest");
    const auto directory = root / "backends.d";
    fs::create_directories(directory / "nested");
    put(directory / "z.json", b.text); put(directory / "a.json", original.text);
    put(directory / "ignored.JSON", "{"); put(directory / "ignored.txt", "{"); put(directory / "nested" / "nested.json", "{");
    auto discovered = discover_backend_definitions(root.string());
    check(discovered.size() == 2 && json::parse(discovered[0].text)["recipe"] == "catalog-alpha" && json::parse(discovered[1].text)["recipe"] == "catalog-beta", "UTF-8 filename sort, immediate lowercase JSON selection");
    fs::permissions(directory / "a.json", fs::perms::none);
    bool unreadable = false;
    try { discover_backend_definitions(root.string()); } catch (const DefinitionValidationError&) { unreadable = true; }
    fs::permissions(directory / "a.json", fs::perms::owner_all);
    check(unreadable, "unreadable selected file rejected");
#ifndef _WIN32
    fs::create_symlink(directory / "a.json", directory / "link.json");
    bool symlink = false;
    try { discover_backend_definitions(root.string()); } catch (const DefinitionValidationError&) { symlink = true; }
    fs::remove(directory / "link.json");
    check(symlink, "selected symlink rejected");
    mkfifo((directory / "pipe.json").c_str(), 0600);
    bool nonregular = false;
    try { discover_backend_definitions(root.string()); } catch (const DefinitionValidationError&) { nonregular = true; }
    fs::remove(directory / "pipe.json");
    check(nonregular, "selected FIFO rejected without opening it");
#endif
    fs::permissions(directory, fs::perms::none);
    bool directory_unreadable = false;
    try { discover_backend_definitions(root.string()); } catch (const DefinitionValidationError&) { directory_unreadable = true; }
    fs::permissions(directory, fs::perms::owner_all);
    check(directory_unreadable, "unreadable directory rejected");
    put(directory / "huge.json", std::string(1024 * 1024 + 1, ' '));
    bool oversize = false;
    try { discover_backend_definitions(root.string()); } catch (const DefinitionValidationError& error) { oversize = error.diagnostics.front().code == "limit_exceeded"; }
    fs::remove(directory / "huge.json");
    check(oversize, "selected definition byte limit");
    fs::rename(directory / "z.json", directory / fs::u8path(u8"雪.json"));
    check(discover_backend_definitions(root.string())[1].file.find(u8"雪.json") != std::string::npos, "UTF-8 filename ordering is locale-independent");
    fs::rename(directory / fs::u8path(u8"雪.json"), directory / "z.json");
    bootstrap_backend_catalog(root.string());
    check(ConfigFile::base_defaults()["catalog-alpha"]["alpha_batch"] == 2, "external defaults on first config access");
    const auto keys = RecipeOptions::known_keys();
    check(std::find(keys.begin(), keys.end(), "alpha_batch") != keys.end(), "external key on first defaults cache access");
    put(directory / "a.json", "{");
    bootstrap_backend_catalog(root.string());
    check(backend_catalog()->export_schema()["catalog_id"] == schema["catalog_id"], "repeated bootstrap does not rescan");
    bool different = false;
    try { bootstrap_backend_catalog(root.string(), true); } catch (const std::logic_error&) { different = true; }
    check(different, "changed bootstrap mode rejected");
    RecipeOptions options("catalog-alpha", {{"alpha_batch", 4}, {"alpha_enabled", false}, {"alpha_text", ""}, {"alpha_bias", -1}});
    check(options.to_json() == json{{"alpha_batch", 4}, {"alpha_enabled", false}, {"alpha_text", ""}, {"alpha_bias", -1}}, "typed false empty string and minus one retained");
    check(RecipeOptions("catalog-alpha", {{"alpha_batch", nullptr}}).get_option("alpha_batch") == 2, "null falls back to declared default");
    for (const auto& value : {json(0), json(9), json(2.0), json("2")}) {
        bool bad = false;
        try { RecipeOptions invalid("catalog-alpha", {{"alpha_batch", value}}); } catch (const std::invalid_argument&) { bad = true; }
        check(bad, "strict typed range and integer validation");
    }
    check(RecipeOptions("catalog-beta", {{"beta_scale", 0}}).get_option("beta_scale") == 0, "zero retained");
    RuntimeConfig config(ConfigFile::base_defaults());
    config.set({{"catalog-alpha", {{"alpha_batch", 5}}}});
    check(config.recipe_options("cpu")["alpha_batch"] == 5, "global nested option path preserves type");
    const auto* a_spec = spec_for("catalog-alpha");
    const auto* b_spec = spec_for("catalog-beta");
    for (int pass = 0; pass < 3; ++pass) {
        auto a_server = create_server("catalog-alpha", {"error"});
        auto b_server = create_server("catalog-beta", {"error"});
        check(a_server->get_descriptor()->recipe == "catalog-alpha" && b_server->get_descriptor()->recipe == "catalog-beta", "interleaved independent factories");
        InstallContext context{*b_spec, b_spec->definition, "catalog-beta", "beta-installer", "beta-pin", "cpu", "requested", "v7", "v9", {"linux", "x86_64", ""}, InstallPhase::Asset};
        const auto params = b_spec->install_params_fn(context);
        check(params.repo == "fixture/beta" && params.filename == "beta-v9.zip" && a_spec->recipe == "catalog-alpha", "independent install context and identities");
    }
    check(external_default_version("beta-pin", "cpu") == "v7" && BackendUtils::get_backend_version("beta-pin", "cpu") == "v7", "external pin fallback");
    RuntimeConfig::set_global(&config);
    BackendManager manager;
    const auto pinned_params = manager.get_install_params("catalog-beta", "cpu");
    check(pinned_params.version == "v7" && pinned_params.filename == "beta-v7.zip", "manager uses external default pin");
    config.set({{"catalog-beta", {{"cpu_bin", "v11"}}}});
    const auto user_params = manager.get_install_params("catalog-beta", "cpu");
    check(user_params.version == "v11" && user_params.filename == "beta-v11.zip", "user version overrides external default pin");
    bool no_download = false;
    try { manager.install_backend("catalog-alpha", "cpu", true); } catch (const std::runtime_error& error) { no_download = std::string(error.what()) == "External-only backend cannot be downloaded"; }
    check(no_download, "direct external-only install rejects before download logic");
    RuntimeConfig::set_global(nullptr);
    Fixture fixture;
    fixture.external = "/fake/catalog-engine";
    fixture.paths.insert("/fake/catalog-engine");
    Services services(fixture);
    ModelInfo model;
    auto inputs = LaunchPlanner().prepare(*a_spec->definition, *a_spec, {"fake", model, RecipeOptions("catalog-alpha", json::object()), "error", false, {}}, services);
    check(LaunchPlanner().plan(*a_spec->definition, inputs).command.args == std::vector<std::string>{"--port", "8123", "--batch", "2"}, "omitted external default reaches launch expression");
    check(std::none_of(fixture.events.begin(), fixture.events.end(), [](const auto& e) { return e.rfind("install:", 0) == 0; }), "external-only never installs");
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
