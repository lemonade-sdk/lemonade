#include "lemon/model_registry.h"
#include "lemon/models/registry_id.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

using lemon::RegistryId;
using lemon::models::descriptor_for;
using lemon::models::huggingface_descriptor;
using lemon::models::modelscope_descriptor;
using lemon::models::RegistryDescriptor;
using lemon::models::RegistryId;
using lemon::models::registry_id_from;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

static void check_eq(const std::string& name, const std::string& actual,
                     const std::string& expected) {
    const bool ok = actual == expected;
    if (!ok) {
        std::printf("       expected \"%s\", got \"%s\"\n", expected.c_str(), actual.c_str());
    }
    check(name, ok);
}

static void test_every_spelling_resolves_to_its_own_host() {
    for (const auto& descriptor : lemon::models::all_registry_descriptors()) {
        for (const auto& alias : descriptor.aliases) {
            const auto resolved = registry_id_from(alias);
            check("alias '" + alias + "' resolves to " + descriptor.id.str(),
                  resolved.has_value() && resolved.value() == descriptor.id);
        }
        std::string mixed = descriptor.id.str();
        if (!mixed.empty()) {
            mixed[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(mixed[0])));
        }
        check("mixed case resolves to " + descriptor.id.str(),
              registry_id_from(mixed).value_or(RegistryId{}) == descriptor.id);
    }
}

static void test_an_unknown_host_resolves_to_nothing() {
    check("an unknown id resolves to nothing", !registry_id_from("gitlfs").has_value());
    check("a near miss resolves to nothing", !registry_id_from("hugging_faces").has_value());
    check("a persisted entry naming an unknown host throws nothing and yields no id",
          !registry_id_from("ModelScop").has_value());
}

static void test_hosts_keep_their_own_namespace_on_disk() {
    const std::string repo = "Qwen/Qwen3-0.6B-GGUF";
    const std::string hf = lemon::registry_repo_cache_dir_name(repo, lemon::models::huggingface_descriptor().id);
    const std::string ms = lemon::registry_repo_cache_dir_name(repo, lemon::models::modelscope_descriptor().id);

    check("one owner/repo cannot collide across hosts", hf != ms);
    check_eq("hugging face keeps its historical directory", hf, "models--Qwen--Qwen3-0.6B-GGUF");
    check_eq("modelscope is namespaced", ms, "modelscope--models--Qwen--Qwen3-0.6B-GGUF");
    check("the directory starts with the host's prefix",
          ms.compare(0, modelscope_descriptor().cache_dir_prefix.size(),
                     modelscope_descriptor().cache_dir_prefix) == 0);
}

static void test_revision_semantics_are_per_host() {
    check_eq("hugging face resolves its default branch", huggingface_descriptor().default_revision,
             "main");
    check_eq("modelscope resolves its default branch", modelscope_descriptor().default_revision,
             "master");
}

// The strings the API and user_models.json persist must come from the table, so
// a host added later needs no change outside it.
static void test_persisted_names_come_from_the_table() {
    check_eq("source name", lemon::remote_registry_source_name(lemon::models::modelscope_descriptor().id),
             modelscope_descriptor().id.str());
    check_eq("display name", lemon::remote_registry_display_name(lemon::models::huggingface_descriptor().id),
             huggingface_descriptor().display_name);
    check_eq("scheme prefix", lemon::remote_registry_scheme_prefix(lemon::models::modelscope_descriptor().id),
             modelscope_descriptor().scheme_prefix);
}

static void test_default_spelling_is_the_default_host() {
    check("an entry with no recorded source is the default host",
          registry_id_from("").value_or(RegistryId{}) == huggingface_descriptor().id);
    check("but naming nothing is not naming a host",
          !lemon::is_remote_registry_source(""));
    check("while an unrecognized name is refused rather than defaulted",
          !registry_id_from("huggingfcae").has_value());
}

static void test_lookup_by_id() {
    check("a known id has a descriptor", descriptor_for(modelscope_descriptor().id) != nullptr);
    check("an unknown id has none", descriptor_for(RegistryId("gitlfs")) == nullptr);
}

int main() {
    test_every_spelling_resolves_to_its_own_host();
    test_an_unknown_host_resolves_to_nothing();
    test_hosts_keep_their_own_namespace_on_disk();
    test_revision_semantics_are_per_host();
    test_persisted_names_come_from_the_table();
    test_default_spelling_is_the_default_host();
    test_lookup_by_id();

    if (g_failures == 0) {
        std::printf("All registry identity tests passed.\n");
    } else {
        std::printf("%d registry identity test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
