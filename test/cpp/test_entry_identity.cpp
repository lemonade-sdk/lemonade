// One binary, the suites below run in order in one process. Each keeps its scratch
// directories and its own failure count; the only shared state is the process-wide
// models directory, which every suite sets on entry.

#include <lemon/model_manager.h>
#include <lemon/models/entry_identity.h>
#include <cstdio>
#include <string>
#include <lemon/canonical_id.h>
#include <lemon/models/catalog.h>
#include <lemon/models/listed_catalog.h>
#include <lemon/models/registry_descriptors.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/path_utils.h>
#include <nlohmann/json.hpp>
#include "support/test_state_isolation.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <vector>
#include "lemon/model_manager.h"
#include "lemon/models/entry_identity.h"


namespace entry_grammar {

using lemon::ModelInfo;
using lemon::ModelSource;
using lemon::models::entry_identity;
using lemon::models::entry_precedence_rank;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static ModelInfo cloud(const std::string& key, const std::string& provider) {
    ModelInfo info;
    info.model_name = key;
    info.recipe = "cloud";
    info.cloud_provider = provider;
    return info;
}

int run() {
    {
        ModelInfo info;
        const auto registered = entry_identity("user.Coder", info);
        check("user. parses to Registered", registered.source == ModelSource::Registered);
        check("user. bare drops the prefix", registered.bare == "Coder");
        check("user. is not self-qualified", !registered.qualifies_itself);

        const auto imported = entry_identity("extra.Scan-Me", info);
        check("extra. parses to Imported", imported.source == ModelSource::Imported);
        check("extra. bare drops the prefix", imported.bare == "Scan-Me");

        const auto builtin = entry_identity("Plain-Model", info);
        check("a bare key is Builtin", builtin.source == ModelSource::Builtin);
        check("a bare key keeps its name", builtin.bare == "Plain-Model");
        check("a bare key is not self-qualified", !builtin.qualifies_itself);
    }

    {
        // A provider listing already qualified the id, and `fireworks.` is none of
        // the handle prefixes: parsing it used to yield Builtin with the whole key
        // as the bare name, which then earned a `builtin.fireworks.X` alias.
        const auto c = entry_identity("fireworks.kimi-k2p5",
                                      cloud("fireworks.kimi-k2p5", "fireworks"));
        check("a cloud entry qualifies itself", c.qualifies_itself);
        check("a cloud entry keeps its qualified name", c.bare == "fireworks.kimi-k2p5");
    }

    {
        ModelInfo plain;
        const auto registered = entry_identity("user.X", plain);
        const auto c = entry_identity("fireworks.X", cloud("fireworks.X", "fireworks"));
        const auto imported = entry_identity("extra.X", plain);
        const auto builtin = entry_identity("X", plain);

        check("a registration outranks a cloud model",
              entry_precedence_rank(registered) < entry_precedence_rank(c));
        check("a cloud model outranks a scanned file",
              entry_precedence_rank(c) < entry_precedence_rank(imported));
        check("a scanned file outranks a built-in",
              entry_precedence_rank(imported) < entry_precedence_rank(builtin));
    }

    if (g_failures == 0) {
        std::printf("All entry identity tests passed.\n");
    } else {
        std::printf("%d entry identity test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

}  // namespace entry_grammar


namespace round_trip {

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::ModelSource;
using nlohmann::json;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

// A GGUF the scanner will take, in the layout it reads a repository from.
void stub_gguf(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    const char header[] = {'G', 'G', 'U', 'F', 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    out.write(header, sizeof(header));
}

ModelInfo row(const std::string& name) {
    ModelInfo info;
    info.model_name = name;
    info.recipe = "llamacpp";
    info.checkpoints["main"] = "org/" + name + ":a.gguf";
    info.suggested = true;
    info.labels = {"chat"};
    return info;
}

}  // namespace

int run() {
    const fs::path scratch = lemon::test::make_scratch_dir("identity_round_trip");
    lemon::utils::set_models_dir((scratch / "models").string());
    const fs::path cache = scratch / "cache";
    const fs::path extra = scratch / "extra";
    fs::create_directories(cache);
    // The registries live in the config dir, which the test sandbox points at HOME.
    lemon::utils::set_config_dir(cache.string());

    {
        std::ofstream out(cache / "user_models.json", std::ios::binary);
        out << R"({"Mine-Model":{"recipe":"llamacpp","checkpoints":{"main":"org/mine:a.gguf"}}})";
    }
    stub_gguf(extra / "models--lemon-test--Ident-Extra-GGUF" / "snapshots" / "active" /
              "Ident-Extra-Q4_K_M.gguf");
    fs::create_directories(extra / "models--lemon-test--Ident-Extra-GGUF" / "refs");
    {
        std::ofstream out(extra / "models--lemon-test--Ident-Extra-GGUF" / "refs" / "main");
        out << "active";
    }

    ModelInfo cloud = row("identprov.Prov-Model");
    cloud.cloud_provider = "identprov";

    ModelManagerConfig config;
    config.cache_dir = cache.string();
    config.extra_models_dir = extra.string();
    config.extra_catalogs = {
        std::make_shared<lemon::models::EnumeratedCatalog>(
            lemon::models::CatalogDescriptor{"identity"},
            [](const lemon::models::CatalogAccumulator&) {
                return std::vector<ModelInfo>{row("Ident-Builtin")};
            }),
        std::make_shared<lemon::models::ListedCatalog>("identprov",
                                                       [cloud] { return std::vector<ModelInfo>{cloud}; }),
    };
    // The identity of a row is the question, not whether this machine could run it.
    config.availability = [](const std::map<std::string, ModelInfo>&) {
        lemon::models::BackendAvailability declared;
        declared.system_ram_gb = 64.0;
        declared.largest_mem_pool_gb = 48.0;
        return declared;
    };
    ModelManager manager(std::move(config));
    // A read that composes; the readers below take the published set as it stands.
    static_cast<void>(manager.get_supported_models());
    const auto models = manager.store().read()->models;
    // Named rows rather than a count: a build box without accelerators publishes
    // fewer built-ins, and that says nothing about identity.
    check("the fixture rows are all published",
          models.count("Ident-Builtin") && models.count("user.Mine-Model") &&
              models.count("identprov.Prov-Model") &&
              std::any_of(models.begin(), models.end(), [](const auto& entry) {
                  return entry.first.find("Ident-Extra") != std::string::npos;
              }));

    bool has_registered = false;
    bool has_imported = false;
    bool has_builtin = false;
    bool qualifies_itself = false;
    bool unparsable_id = false;
    bool bare_mismatch = false;
    bool reserved_mismatch = false;
    std::set<std::string> idempotent_ok;
    for (const auto& [key, info] : models) {
        const lemon::models::EntryIdentity identity = lemon::models::entry_identity(key, info);
        qualifies_itself = qualifies_itself || identity.qualifies_itself;
        const std::string id = lemon::cache_key_to_canonical_id(key);
        const auto parsed = lemon::parse_canonical_id(id);
        if (!parsed || parsed->full() != id) {
            unparsable_id = true;
            std::printf("    [%s] minted %s\n", key.c_str(), id.c_str());
            continue;
        }
        if (parsed->bare_name != identity.bare) bare_mismatch = true;
        if (parsed->source == ModelSource::Registered) has_registered = true;
        if (parsed->source == ModelSource::Imported) has_imported = true;
        if (parsed->source == ModelSource::Builtin) has_builtin = true;
        // What a listing mints for one source must be creatable by the flow that
        // owns that source, and refused by the flows that do not.
        if (lemon::is_reserved_registration_name(id) !=
            (parsed->source != ModelSource::Registered)) {
            reserved_mismatch = true;
            std::printf("    [%s] reserved for %d\n", id.c_str(), static_cast<int>(parsed->source));
        }
        if (lemon::cache_key_to_canonical_id(id) == id) idempotent_ok.insert(key);
    }

    check("the fixture set covers a registered row", has_registered);
    check("a discovered row", has_imported);
    check("a built-in row", has_builtin);
    check("and a provider that qualifies its own id", qualifies_itself);
    check("every minted id parses back to itself", !unparsable_id);
    check("and its bare name survives the prefix", !bare_mismatch);
    check("only a registered id is registrable", !reserved_mismatch);
    check("minting an id twice changes nothing", idempotent_ok.size() == models.size());

    bool name_escapes = false;
    std::map<std::string, std::string> listed;
    for (const auto& [key, info] : models) {
        static_cast<void>(info);
        const std::string public_name = manager.get_public_model_name(key);
        if (public_name.empty()) continue;
        if (listed.count(public_name) && listed[public_name] != key) {
            name_escapes = true;
            std::printf("    [%s] shares %s with %s\n", key.c_str(), public_name.c_str(),
                        listed[public_name].c_str());
        }
        listed[public_name] = key;
        const auto resolved = manager.resolve(public_name);
        if (!resolved || resolved->canonical_id != key) {
            name_escapes = true;
            std::printf("    [%s] listed as %s, resolves to %s\n", key.c_str(), public_name.c_str(),
                        resolved ? resolved->canonical_id.c_str() : "<nothing>");
        }
    }
    check("every listed name answers with the row it names", !name_escapes);

    check("a bare name is not a canonical id",
          !lemon::parse_canonical_id("My-Model").has_value());
    check("a registered id is registrable",
          !lemon::is_reserved_registration_name("user.My-Model"));
    check("a discovered id is not", lemon::is_reserved_registration_name("extra.My-Model"));
    check("a built-in id is not", lemon::is_reserved_registration_name("builtin.My-Model"));
    check("nor is a registered id hiding one",
          lemon::is_reserved_registration_name("user.extra.My-Model") &&
              lemon::is_reserved_registration_name("user.builtin.My-Model"));

    if (g_failures > 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll identity round-trip checks passed\n");
    return 0;
}

}  // namespace round_trip

// The two name tables a store build derives from its model set: which name wins,
// what a client is answered with, and what a shadowed entry falls back to.
namespace name_tables {

// The two name tables a store build derives from its model set: which name wins,
// what a client is answered with, and what a shadowed entry falls back to.



using lemon::ModelInfo;
using lemon::models::ModelAliasMaps;
using lemon::models::compute_model_alias_maps;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static ModelInfo model(const std::string& key, const std::string& recipe = "llamacpp") {
    ModelInfo info;
    info.model_name = key;
    info.recipe = recipe;
    return info;
}

static void test_precedence() {
    ModelAliasMaps against_builtin = compute_model_alias_maps(
        {{"Foo", model("Foo")}, {"user.Foo", model("user.Foo")}});
    check("a registration takes the bare name from a built-in",
          against_builtin.public_model_aliases["Foo"] == "user.Foo");
    check("the registration is listed under the bare name",
          against_builtin.canonical_public_names["user.Foo"] == "Foo");
    check("the shadowed built-in is listed under its canonical id",
          against_builtin.canonical_public_names["Foo"] == "builtin.Foo" &&
              against_builtin.public_model_aliases["builtin.Foo"] == "Foo");

    ModelAliasMaps against_scan = compute_model_alias_maps(
        {{"extra.Foo", model("extra.Foo")}, {"user.Foo", model("user.Foo")}});
    check("a registration outranks a scanned file",
          against_scan.public_model_aliases["Foo"] == "user.Foo");
    check("the shadowed scan keeps the id it was minted with",
          against_scan.canonical_public_names["extra.Foo"] == "extra.Foo");
}

static void test_input_aliases() {
    ModelInfo claiming = model("Foo");
    claiming.input_aliases = {"bar"};
    ModelAliasMaps demotion =
        compute_model_alias_maps({{"bar", model("bar")}, {"Foo", claiming}});
    check("an unqualified built-in yields its bare name to an input alias",
          demotion.public_model_aliases["bar"] == "Foo");
    check("and is reachable under the id it is pushed to",
          demotion.canonical_public_names["bar"] == "builtin.bar" &&
              demotion.public_model_aliases["builtin.bar"] == "bar");

    ModelInfo cloud = model("fireworks.x");
    cloud.cloud_provider = "fireworks";
    ModelInfo claims_cloud = model("Foo");
    claims_cloud.input_aliases = {"fireworks.x"};
    ModelAliasMaps self_qualified = compute_model_alias_maps(
        {{"fireworks.x", cloud}, {"Foo", claims_cloud}});
    check("a provider-qualified id is not pushed aside by an input alias",
          self_qualified.public_model_aliases["fireworks.x"] == "fireworks.x");
    check("and is never re-prefixed as a built-in",
          self_qualified.public_model_aliases.count("builtin.fireworks.x") == 0 &&
              self_qualified.canonical_public_names.count("builtin.fireworks.x") == 0);

    ModelInfo aliased = model("Foo");
    aliased.input_aliases = {"user.Other"};
    ModelAliasMaps canonical_alias = compute_model_alias_maps({{"Foo", aliased}});
    check("an input alias naming a canonical id resolves to its target",
          canonical_alias.public_model_aliases["user.Other"] == "Foo");
}

static void test_collections() {
    // A registered collection must be listed under the id its user registered,
    // fetched and chatted it by, not under a bare name that names nothing else.
    ModelAliasMaps registered_collection = compute_model_alias_maps(
        {{"user.MyColl", model("user.MyColl", "collection.omni")}});
    check("a registered collection lists under its canonical id",
          registered_collection.canonical_public_names["user.MyColl"] == "user.MyColl");

    ModelAliasMaps builtin_collection = compute_model_alias_maps(
        {{"TheirColl", model("TheirColl", "collection.omni")}});
    check("a built-in collection lists under its bare name",
          builtin_collection.canonical_public_names["TheirColl"] == "TheirColl");
}

int run() {
    test_precedence();
    test_input_aliases();
    test_collections();

    if (g_failures == 0) {
        std::printf("All alias map tests passed.\n");
    } else {
        std::printf("%d alias map test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

}  // namespace name_tables

int main() {
    int failures = 0;
    std::printf("\n=== entry grammar ===\n");
    failures |= entry_grammar::run();
    std::printf("\n=== round trip ===\n");
    failures |= round_trip::run();
    std::printf("\n=== name tables ===\n");
    failures |= name_tables::run();
    return failures;
}
