#include "lemon/models/catalog.h"
#include "lemon/models/artifact_ref.h"
#include "lemon/models/listed_catalog.h"
#include "lemon/models/model_index.h"
#include "lemon/models/registry_id.h"

#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using lemon::models::ModelIndex;
using lemon::models::RegistryId;

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

static void test_checkpoint_grammar() {
    check_eq("repo id is the text before the colon",
             lemon::models::repo_id_from_checkpoint("org/repo:weight.gguf"), "org/repo");
    check_eq("variant is the text after the colon",
             lemon::models::variant_from_checkpoint("org/repo:weight.gguf"), "weight.gguf");
    check_eq("a bare repo has no variant",
             lemon::models::variant_from_checkpoint("org/repo"), "");
    check_eq("a bare repo keeps its whole name",
             lemon::models::repo_id_from_checkpoint("org/repo"), "org/repo");

    // The split is on the first colon only, which is what the cache dir lookup
    // has always assumed.
    check_eq("split happens at the first colon",
             lemon::models::repo_id_from_checkpoint("org/repo:a:b"), "org/repo");
    check_eq("the remainder keeps later colons",
             lemon::models::variant_from_checkpoint("org/repo:a:b"), "a:b");
}

static void test_name_reserving() {
    const std::string prefix = "extra.";
    std::set<std::string> claimed;
    const std::set<std::string> none;

    check_eq("the bare name wins when free",
             lemon::models::reserve_unique_name(claimed, none, prefix, "Foo", "dir"), "Foo");
    check("the claim is recorded", claimed.count("Foo") == 1);

    // Same bare name from another directory: qualified rather than dropped.
    check_eq("a collision is qualified, not dropped",
             lemon::models::reserve_unique_name(claimed, none, prefix, "Foo", "beta"),
             "beta-Foo");
    check_eq("the second collision is numbered",
             lemon::models::reserve_unique_name(claimed, none, prefix, "Foo", "beta"),
             "beta-Foo-2");
    check_eq("the third collision is numbered again",
             lemon::models::reserve_unique_name(claimed, none, prefix, "Foo", "beta"),
             "beta-Foo-3");

    // Claiming is case sensitive: a different casing is a different model.
    check_eq("a different casing claims the bare name",
             lemon::models::reserve_unique_name(claimed, none, prefix, "foo", "gamma"), "foo");
}

static void test_reserved_ids_are_prefixed() {
    const std::string prefix = "extra.";
    std::set<std::string> claimed;
    const std::set<std::string> reserved = {"extra.chat", "extra.embeddings"};

    check_eq("a reserved id cannot be claimed bare",
             lemon::models::reserve_unique_name(claimed, reserved, prefix, "chat", "gamma"),
             "gamma-chat");
    check("the reserved id stays free for its owner", claimed.count("chat") == 0);
    check_eq("an unreserved name is unaffected",
             lemon::models::reserve_unique_name(claimed, reserved, prefix, "plain", "gamma"),
             "plain");
}

static void test_model_index_ownership() {
    ModelIndex index;
    check("a first insert is accepted",
          index.add("Foo", "builtin.Foo") == ModelIndex::InsertResult::Inserted);
    check("the name resolves", index.canonical_for("Foo") == "builtin.Foo");
    check("the id is owned", index.owns("builtin.Foo"));

    check("the same id again is a duplicate, not a shadow",
          index.add("Foo", "builtin.Foo") == ModelIndex::InsertResult::DuplicateId);

    check("a second owner of one name is shadowed",
          index.add("Foo", "extra.Foo") == ModelIndex::InsertResult::Shadowed);
    check_eq("the first owner keeps the name", index.canonical_for("Foo"), "builtin.Foo");
    check("the shadowed id is not owned", !index.owns("extra.Foo"));
    check("shadowing does not grow the index", index.size() == 1);

    check("a distinct name is accepted",
          index.add("Bar", "user.Bar") == ModelIndex::InsertResult::Inserted);
    check("size counts both", index.size() == 2);
    check("an unknown name resolves to nothing", index.canonical_for("Nope").empty());
}

static void test_index_is_case_sensitive() {
    ModelIndex index;
    index.add("Foo", "builtin.Foo");

    check("storage is case sensitive", !index.contains("foo"));
    check_eq("a differently cased lookup finds nothing", index.canonical_for("foo"), "");

    const std::vector<std::string> matches = index.names_ignoring_case("FOO");
    check("case-insensitive lookup finds one name", matches.size() == 1);
    check_eq("case-insensitive lookup returns the stored spelling",
             matches.empty() ? "" : matches.front(), "Foo");
}

static lemon::ModelInfo model(const std::string& name, const std::string& recipe) {
    lemon::ModelInfo info;
    info.model_name = name;
    info.recipe = recipe;
    return info;
}

using lemon::models::CatalogAccumulator;
using lemon::models::CatalogEntry;
using lemon::models::CatalogDescriptor;
using lemon::models::EnumeratedCatalog;
using lemon::models::ListedCatalog;

typedef std::vector<lemon::ModelInfo> Models;

static void test_claimed_names_survive_later_catalogs() {
    CatalogAccumulator acc;
    acc.add(CatalogEntry{"registry", model("Dup", "llamacpp")});
    const auto outcome = acc.add(CatalogEntry{"extra_models_dir", model("Dup", "sd-cpp")});

    check("a later catalog cannot take a claimed name",
          outcome == CatalogAccumulator::Outcome::KeptExisting);
    check_eq("the first entry survives", acc.models().at("Dup").recipe, "llamacpp");
}

static void test_overwrite_replaces_in_place() {
    CatalogAccumulator acc;
    acc.add(CatalogEntry{"registry", model("Dup", "llamacpp")});
    acc.add(CatalogEntry{"user_models.json", model("Dup", "sd-cpp")},
            CatalogAccumulator::Conflict::Overwrite);

    check_eq("overwrite replaces the definition", acc.models().at("Dup").recipe, "sd-cpp");
    check("overwrite does not add a second entry", acc.size() == 1);
}

static void test_names_already_claimed_before_enumeration() {
    std::map<std::string, lemon::ModelInfo> claimed;
    claimed["Registered"] = model("Registered", "llamacpp");
    CatalogAccumulator acc(std::move(claimed));

    const auto outcome = acc.add(CatalogEntry{"extra_models_dir", model("Registered", "sd-cpp")});
    check("a name claimed before enumeration wins",
          outcome == CatalogAccumulator::Outcome::KeptExisting);
    check_eq("and its definition stands", acc.models().at("Registered").recipe, "llamacpp");
}

static void test_catalog_is_told_what_earlier_ones_claimed() {
    CatalogAccumulator acc;
    acc.add(CatalogEntry{"registry", model("Taken", "llamacpp")});

    std::vector<std::string> seen;
    EnumeratedCatalog catalog(CatalogDescriptor{"extra_models_dir"}, [&](const CatalogAccumulator& so_far) -> Models {
        for (const auto& entry : so_far.models()) {
            seen.push_back(entry.first);
        }
        return {model("Fresh", "llamacpp")};
    });
    catalog.enumerate(acc);

    check("the callable sees earlier claims", seen.size() == 1 && seen[0] == "Taken");
    check("and contributes its own", acc.contains("Fresh"));
}

static void test_catalog_reports_only_the_entries_it_lost() {
    CatalogAccumulator acc;
    acc.add(CatalogEntry{"registry", model("Taken", "llamacpp")});

    std::vector<std::string> conflicts;
    EnumeratedCatalog catalog(CatalogDescriptor{"extra_models_dir"},
                              [](const CatalogAccumulator&) {
                                  return Models{model("Taken", "sd-cpp"),
                                                model("Fresh", "sd-cpp")};
                              },
                              [&](const std::string& name) { conflicts.push_back(name); });
    catalog.enumerate(acc);

    check("the lost entry is reported", conflicts.size() == 1 && conflicts[0] == "Taken");
    check("the winner is untouched", acc.models().at("Taken").recipe == "llamacpp");
}

static void test_a_failing_source_does_not_take_the_set_down_with_it() {
    CatalogAccumulator optional;
    optional.add(CatalogEntry{"registry", model("Kept", "llamacpp")});
    EnumeratedCatalog unreadable(CatalogDescriptor{"extra_models_dir"},
                                 [](const CatalogAccumulator&) -> Models {
                                     throw std::runtime_error("unreadable directory");
                                 });
    unreadable.enumerate(optional);
    check("an unreadable optional source costs only its own models", optional.size() == 1);

    CatalogAccumulator required;
    EnumeratedCatalog registry(
        CatalogDescriptor{"server_models.json", true},
        [](const CatalogAccumulator&) -> Models {
            throw std::runtime_error("unreadable resource");
        });
    bool threw = false;
    try {
        registry.enumerate(required);
    } catch (const std::exception&) {
        threw = true;
    }
    check("a required source still fails the build", threw);
}

static void test_a_listing_names_the_provider_it_came_from() {
    const ListedCatalog catalog("fireworks",
                                [] { return Models{model("fireworks.deepseek-v4-pro", "cloud")}; });
    check_eq("the catalog id names the provider", catalog.id(), "listed:fireworks");
}

static void test_a_provider_that_will_not_answer_costs_only_its_own_models() {
    CatalogAccumulator acc;
    acc.add(CatalogEntry{"registry", model("Kept", "llamacpp")});

    const ListedCatalog silent(
        "fireworks", []() -> Models { throw std::runtime_error("connection refused"); });
    bool threw = false;
    try {
        silent.enumerate(acc);
    } catch (...) {
        threw = true;
    }
    check("an unreachable provider does not fail the build", !threw);
    check("it contributes nothing", acc.size() == 1);

    // The point of one catalog per provider: a second provider still lists.
    const ListedCatalog answering("openrouter",
                                  [] { return Models{model("openrouter.llama-70b", "cloud")}; });
    answering.enumerate(acc);
    check("another provider is unaffected", acc.contains("openrouter.llama-70b"));
}

int main() {
    test_checkpoint_grammar();
    test_name_reserving();
    test_reserved_ids_are_prefixed();
    test_model_index_ownership();
    test_index_is_case_sensitive();
    test_claimed_names_survive_later_catalogs();
    test_overwrite_replaces_in_place();
    test_names_already_claimed_before_enumeration();
    test_catalog_is_told_what_earlier_ones_claimed();
    test_catalog_reports_only_the_entries_it_lost();
    test_a_failing_source_does_not_take_the_set_down_with_it();
    test_a_listing_names_the_provider_it_came_from();
    test_a_provider_that_will_not_answer_costs_only_its_own_models();

    if (g_failures == 0) {
        std::printf("All model catalog tests passed.\n");
    } else {
        std::printf("%d model catalog test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
