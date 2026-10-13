#include "lemon/models/catalog.h"
#include "lemon/models/locator.h"
#include "lemon/models/model_index.h"

#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using lemon::RegistryId;
using lemon::models::Locator;
using lemon::models::LocatorScheme;
using lemon::models::ModelIndex;

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

static void test_registry_locator() {
    const Locator locator = Locator::from_checkpoint("org/repo:weight.gguf", "");
    check("an empty source is a registry model", locator.is_registry());
    check_eq("repo id", locator.repo_id(), "org/repo");
    check_eq("variant", locator.variant, "weight.gguf");
    check("the registry resolves to the default provider",
          locator.registry && *locator.registry == lemon::models::huggingface_descriptor().id);
    check_eq("renders as hf://repo:variant", locator.to_string(), "hf://org/repo:weight.gguf");

    const Locator bare = Locator::from_checkpoint("org/repo", "");
    check_eq("a repo without a variant renders without a colon", bare.to_string(),
             "hf://org/repo");
}

static void test_local_schemes_come_from_source() {
    // A Windows absolute path contains a colon. Deriving the scheme from the
    // path's shape would read this as a repo named "C" plus a variant.
    const std::string windows_path = "C:\\models\\foo.gguf";
    const Locator imported = Locator::from_checkpoint(windows_path, "local_path");
    check("local_path is an import", imported.is_import());
    check_eq("the path survives intact", imported.path, windows_path);
    check_eq("an import has no variant", imported.variant, "");
    check_eq("import renders with its scheme", imported.to_string(), "import://" + windows_path);

    const Locator uploaded = Locator::from_checkpoint("models--org--repo/x.gguf", "local_upload");
    check("local_upload is cache relative", uploaded.is_cache());
    check_eq("the cache relative path survives intact", uploaded.path, "models--org--repo/x.gguf");
    check_eq("cache renders with its scheme", uploaded.to_string(),
             "local://models--org--repo/x.gguf");

    // A POSIX absolute path looks like a repo to a shape-based parser too.
    const Locator posix = Locator::from_checkpoint("/home/u/models/foo.gguf", "local_path");
    check("a posix absolute path is an import", posix.is_import());
    check_eq("a posix absolute path survives intact", posix.path, "/home/u/models/foo.gguf");

    check("a registry locator reports no import path",
          Locator::from_checkpoint("org/repo", "").repo_id() == "org/repo");
}

static void test_registry_is_named() {
    const Locator other = Locator::from_checkpoint("org/repo:model.gguf", "modelscope");
    check("a non-default source is still a registry model", other.is_registry());
    check("the registry is remembered", other.registry &&
          *other.registry == lemon::models::modelscope_descriptor().id);
    check_eq("the registry keeps its own prefix", other.to_string(),
             "ms://org/repo:model.gguf");

    const Locator url = Locator::from_checkpoint("https://example.com/model.gguf", "url");
    check("url is its own scheme", url.is_url());
    check_eq("an absolute url is not double prefixed", url.to_string(),
             "https://example.com/model.gguf");
}

static void test_source_aliases_are_normalized() {
    // The server already accepts these spellings; a locator that carried the
    // raw string through would render them as their own registry.
    const char* aliases[] = {"hf", "HF", "huggingface", "HuggingFace"};
    for (const char* alias : aliases) {
        const Locator locator = Locator::from_checkpoint("org/repo", alias);
        check(std::string(alias) + " normalizes to the default provider",
              locator.registry && *locator.registry == lemon::models::huggingface_descriptor().id);
        check_eq(std::string(alias) + " renders the canonical prefix", locator.to_string(),
                 "hf://org/repo");
    }

    const Locator short_other = Locator::from_checkpoint("org/repo", "MS");
    check("the short alias for the other registry normalizes",
          short_other.registry && *short_other.registry == lemon::models::modelscope_descriptor().id);
}

static void test_unknown_source_is_rejected() {
    bool threw = false;
    try {
        Locator::from_checkpoint("org/repo", "not-a-registry");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check("an unknown registry is refused rather than defaulted", threw);
}

static void test_path_locators_carry_no_registry() {
    const Locator imported = Locator::from_checkpoint("/srv/models/foo.gguf", "local_path");
    check("an import names no registry", !imported.registry.has_value());
    const Locator uploaded = Locator::from_checkpoint("models--org--repo/x.gguf", "local_upload");
    check("a cache model names no registry", !uploaded.registry.has_value());
    const Locator url = Locator::from_checkpoint("https://example.com/x.gguf", "url");
    check("a url names no registry", !url.registry.has_value());
}

static void test_revision_is_additive() {
    Locator locator = Locator::from_checkpoint("org/repo", "");
    check_eq("no revision is parsed today", locator.revision, "");
    check_eq("without a revision nothing is appended", locator.to_string(), "hf://org/repo");
    locator.revision = "deadbeef";
    check_eq("a revision is carried through", locator.to_string(), "hf://org/repo@deadbeef");
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
using lemon::models::EnumeratedCatalog;

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
    EnumeratedCatalog catalog("extra_models_dir", [&](const CatalogAccumulator& so_far) -> Models {
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
    EnumeratedCatalog catalog("extra_models_dir",
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
    EnumeratedCatalog unreadable("extra_models_dir",
                                 [](const CatalogAccumulator&) -> Models {
                                     throw std::runtime_error("unreadable directory");
                                 });
    unreadable.enumerate(optional);
    check("an unreadable optional source costs only its own models", optional.size() == 1);

    CatalogAccumulator required;
    EnumeratedCatalog registry("server_models.json",
                               [](const CatalogAccumulator&) -> Models {
                                   throw std::runtime_error("unreadable resource");
                               },
                               nullptr, /*required=*/true);
    bool threw = false;
    try {
        registry.enumerate(required);
    } catch (const std::exception&) {
        threw = true;
    }
    check("a required source still fails the build", threw);
}

int main() {
    test_checkpoint_grammar();
    test_registry_locator();
    test_local_schemes_come_from_source();
    test_registry_is_named();
    test_source_aliases_are_normalized();
    test_unknown_source_is_rejected();
    test_path_locators_carry_no_registry();
    test_revision_is_additive();
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

    if (g_failures == 0) {
        std::printf("All model catalog tests passed.\n");
    } else {
        std::printf("%d model catalog test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
