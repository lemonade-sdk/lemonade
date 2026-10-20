#include "lemon/prometheus_metrics.h"

#include <cstdio>
#include <string>
#include <unordered_map>

using lemon::build_metrics_alias_map;
using lemon::MetricsAliasMap;

struct TestResult {
    int passed = 0;
    int failed = 0;

    void check(bool condition, const std::string& name) {
        printf("[%s] %s\n", condition ? "PASS" : "FAIL", name.c_str());
        if (condition) {
            ++passed;
        } else {
            ++failed;
        }
    }
};

// Stands in for AliasManager::resolve_alias followed by the public-name lookup:
// a chain walk that yields an empty string for anything unresolvable.
static std::string fake_resolve(
        const std::unordered_map<std::string, std::string>& aliases,
        const std::string& alias) {
    std::string current = alias;
    for (int hop = 0; hop < 10; ++hop) {
        auto it = aliases.find(current);
        if (it == aliases.end()) {
            return current == alias ? "" : current;
        }
        if (it->second == alias) return "";
        current = it->second;
    }
    return "";
}

static MetricsAliasMap map_for(const std::unordered_map<std::string, std::string>& aliases) {
    return build_metrics_alias_map(aliases, [&aliases](const std::string& alias) {
        return fake_resolve(aliases, alias);
    });
}

static void test_alias_maps_to_target(TestResult& r) {
    MetricsAliasMap mapping = map_for({{"coding", "Qwen3-8B-GGUF"}});

    r.check(mapping.size() == 1, "one entry per resolvable alias");
    r.check(mapping["coding"] == "Qwen3-8B-GGUF", "alias maps to its target model");
}

static void test_chained_alias_reports_final_target(TestResult& r) {
    MetricsAliasMap mapping = map_for({{"hop", "coding"}, {"coding", "Qwen3-8B-GGUF"}});

    r.check(mapping.size() == 2, "every alias in a chain is exposed");
    r.check(mapping["hop"] == "Qwen3-8B-GGUF",
            "a chained alias maps to the model, not to the intermediate alias");
}

static void test_multiple_aliases_share_a_target(TestResult& r) {
    MetricsAliasMap mapping =
        map_for({{"coding", "Qwen3-8B-GGUF"}, {"default", "Qwen3-8B-GGUF"}});

    r.check(mapping.size() == 2, "both aliases are exposed");
    r.check(mapping["coding"] == mapping["default"],
            "aliases of one model agree on the target");
}

static void test_unresolvable_aliases_are_dropped(TestResult& r) {
    // A cycle is what AliasManager reports as unresolvable; /metrics must not
    // advertise a mapping that inference would refuse to follow.
    r.check(map_for({{"a", "b"}, {"b", "a"}}).empty(), "cyclic aliases are dropped");
    r.check(map_for({}).empty(), "no aliases produces no mapping");

    MetricsAliasMap no_resolver = build_metrics_alias_map({{"coding", "Qwen3-8B-GGUF"}}, nullptr);
    r.check(no_resolver.empty(), "a missing resolver produces no mapping");

    MetricsAliasMap empty_key = build_metrics_alias_map(
        {{"", "Qwen3-8B-GGUF"}},
        [](const std::string&) { return std::string("Qwen3-8B-GGUF"); });
    r.check(empty_key.empty(), "an empty alias name is dropped");

    MetricsAliasMap self = build_metrics_alias_map(
        {{"coding", "coding"}},
        [](const std::string& alias) { return alias; });
    r.check(self.empty(), "an alias resolving to itself is dropped");
}

int main() {
    TestResult r;

    test_alias_maps_to_target(r);
    test_chained_alias_reports_final_target(r);
    test_multiple_aliases_share_a_target(r);
    test_unresolvable_aliases_are_dropped(r);

    printf("\n%d passed, %d failed\n", r.passed, r.failed);
    return r.failed == 0 ? 0 : 1;
}
