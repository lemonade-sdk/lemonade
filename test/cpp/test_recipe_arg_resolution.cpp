#include "lemon/backends/llamacpp/llamacpp.h"
#include "lemon/utils/recipe_arg_resolver.h"

#include <cstdio>
#include <optional>
#include <set>
#include <string>
#include <vector>

using lemon::KvCacheQuantTier;
using lemon::backends::llamacpp::kv_cache_type_launch_args;
using lemon::utils::CustomArgsRequestState;
using lemon::utils::RuntimeArgDefault;
using lemon::utils::ScopedCustomArgs;
using lemon::utils::append_runtime_arg_defaults;
using lemon::utils::build_custom_args_map;
using lemon::utils::is_custom_args_option;
using lemon::utils::parse_custom_args;
using lemon::utils::resolve_scoped_custom_args;
using lemon::utils::validate_custom_args;

static bool expect_args(const char* name, const std::string& actual,
                        const std::string& expected) {
    const auto actual_map = build_custom_args_map(parse_custom_args(actual));
    const auto expected_map = build_custom_args_map(parse_custom_args(expected));
    const bool ok = actual_map == expected_map;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  %s\n  want: %s\n", actual.c_str(), expected.c_str());
    }
    return ok;
}

static bool expect_bool(const char* name, bool actual, bool expected) {
    const bool ok = actual == expected;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        std::printf("  got:  %s\n  want: %s\n",
                    actual ? "true" : "false",
                    expected ? "true" : "false");
    }
    return ok;
}

int main() {
    int failures = 0;

    failures += !expect_bool(
        "llamacpp_args is a custom args option",
        is_custom_args_option("llamacpp_args"), true);
    failures += !expect_bool(
        "sdcpp_args is a custom args option",
        is_custom_args_option("sdcpp_args"), true);
    failures += !expect_bool(
        "merge_args is not a custom args option",
        is_custom_args_option("merge_args"), false);

    const std::string backend = "-b 2048 -ub 1024 -np 1";
    const std::string architecture = "--temp 0.8 --top-k 40";
    const std::string model_defaults = "--flash-attn on";
    const std::string model = "--no-mmap --threads 8";

    failures += !expect_args(
        "no request inherits backend and model scope",
        resolve_scoped_custom_args(
            {backend, architecture, model_defaults, model,
             CustomArgsRequestState::Omitted, "", true}),
        "-b 2048 -ub 1024 -np 1 --temp 0.8 --top-k 40 --no-mmap --threads 8");

    failures += !expect_args(
        "request replaces model scope and keeps backend",
        resolve_scoped_custom_args(
            {backend, architecture, model_defaults, model,
             CustomArgsRequestState::Value, "--threads 4", true}),
        "-b 2048 -ub 1024 -np 1 --threads 4");

    failures += !expect_args(
        "empty request clears model scope and keeps backend",
        resolve_scoped_custom_args(
            {backend, architecture, model_defaults, model,
             CustomArgsRequestState::Value, "", true}),
        backend);

    failures += !expect_args(
        "null tombstone skips saved args and falls through to defaults",
        resolve_scoped_custom_args(
            {backend, architecture, model_defaults, model,
             CustomArgsRequestState::Tombstone, "", true}),
        "-b 2048 -ub 1024 -np 1 --temp 0.8 --top-k 40 --flash-attn on");

    failures += !expect_args(
        "merge false without request inherits no custom args",
        resolve_scoped_custom_args(
            {backend, architecture, model_defaults, model,
             CustomArgsRequestState::Omitted, "", false}),
        "");

    failures += !expect_args(
        "merge false with request uses request only",
        resolve_scoped_custom_args(
            {backend, architecture, model_defaults, model,
             CustomArgsRequestState::Value, "--threads 4", false}),
        "--threads 4");

    failures += !expect_args(
        "merge false with null tombstone inherits no custom args",
        resolve_scoped_custom_args(
            {backend, architecture, model_defaults, model,
             CustomArgsRequestState::Tombstone, "", false}),
        "");

    failures += !expect_args(
        "request overrides same backend flag with equals syntax",
        resolve_scoped_custom_args(
            {"--threads 12 -b 2048", "", "", "",
             CustomArgsRequestState::Value, "--threads=4", true}),
        "-b 2048 --threads 4");

    failures += !expect_args(
        "request negation overrides backend opposite",
        resolve_scoped_custom_args(
            {"--mmap -b 2048", "", "", "",
             CustomArgsRequestState::Value, "--no-mmap", true}),
        "-b 2048 --no-mmap");

    failures += !expect_args(
        "repeatable request flags survive wholesale model replacement",
        resolve_scoped_custom_args(
            {"-b 2048", "--threads 8", "", "--threads 6",
             CustomArgsRequestState::Value,
             "--override-kv a=bool:false --override-kv b=bool:false", true}),
        "-b 2048 --override-kv a=bool:false --override-kv b=bool:false");

    const std::vector<RuntimeArgDefault> runtime_defaults = {
        {"--spec-type draft-mtp", "--spec-type"},
    };

    failures += !expect_args(
        "runtime default becomes part of effective args",
        append_runtime_arg_defaults("--threads 4", runtime_defaults),
        "--threads 4 --spec-type draft-mtp");

    failures += !expect_args(
        "explicit runtime option overrides default",
        append_runtime_arg_defaults("--spec-type=draft-dflash", runtime_defaults),
        "--spec-type draft-dflash");

    failures += !expect_args(
        "merge false suppresses overridable runtime defaults",
        append_runtime_arg_defaults("--threads 4", runtime_defaults, false),
        "--threads 4");

    const std::vector<RuntimeArgDefault> parallel_defaults = {
        {"--parallel 1", "--parallel", {"-np"}},
    };

    failures += !expect_args(
        "runtime default with no override becomes part of effective args",
        append_runtime_arg_defaults("--threads 4", parallel_defaults),
        "--threads 4 --parallel 1");

    failures += !expect_args(
        "explicit flag overrides runtime default",
        append_runtime_arg_defaults("--parallel 4", parallel_defaults),
        "--parallel 4");

    failures += !expect_args(
        "explicit alias overrides runtime default",
        append_runtime_arg_defaults("-np 4", parallel_defaults),
        "-np 4");


    // --- U6: cache-type-k/v launch args for a resolved KV cache tier ---

    {
        auto f16 = kv_cache_type_launch_args(KvCacheQuantTier::F16);
        failures += !expect_bool("tier f16: no cache-type argv", f16.argv.empty(), true);
        failures += !expect_bool("tier f16: no cache-type reservations", f16.reservations.empty(), true);
    }
    {
        auto unresolved = kv_cache_type_launch_args(std::nullopt);
        failures += !expect_bool("unresolved tier: no cache-type argv", unresolved.argv.empty(), true);
    }
    {
        auto q8 = kv_cache_type_launch_args(KvCacheQuantTier::Q8_0);
        const std::vector<std::string> expected = {"--cache-type-k", "q8_0", "--cache-type-v", "q8_0"};
        failures += !expect_bool("tier q8_0: both flags emitted with the same value",
            q8.argv == expected, true);
        failures += !expect_bool("tier q8_0: both flags reserved",
            q8.reservations.size() == 2, true);
    }

    // Mirror LlamaCppServer::load()'s reserved-flag construction to prove the
    // managed-flag collision fires for the cache-type flags exactly as it
    // does for every other managed flag.
    {
        std::set<std::string> reserved = {"-m", "--model", "--ctx-size", "-c"};
        auto q8 = kv_cache_type_launch_args(KvCacheQuantTier::Q8_0);
        for (auto& [flag, alias] : q8.reservations) {
            reserved.insert(flag);
            reserved.insert(alias);
        }
        failures += !expect_bool("tier q8_0 with --cache-type-k q4_0 passthrough: collision detected",
            !validate_custom_args("--cache-type-k q4_0", reserved).empty(), true);
        failures += !expect_bool("tier q8_0 with -ctk q4_0 passthrough: collision detected (K alias)",
            !validate_custom_args("-ctk q4_0", reserved).empty(), true);
        failures += !expect_bool("tier q8_0 with -ctv q4_0 passthrough: collision detected (V alias)",
            !validate_custom_args("-ctv q4_0", reserved).empty(), true);
        failures += !expect_bool("tier q8_0 with no flash-attention arg: -fa not reserved",
            reserved.count("-fa") == 0 && reserved.count("--flash-attn") == 0, true);
    }
    {
        // f16: --cache-type-k in the passthrough reaches argv unchanged —
        // today's behavior, preserved (KTD7).
        std::set<std::string> reserved = {"-m", "--model", "--ctx-size", "-c"};
        auto f16 = kv_cache_type_launch_args(KvCacheQuantTier::F16);
        for (auto& [flag, alias] : f16.reservations) {
            reserved.insert(flag);
            reserved.insert(alias);
        }
        failures += !expect_bool("tier f16 with --cache-type-k in passthrough: no collision",
            validate_custom_args("--cache-type-k q8_0", reserved).empty(), true);
    }
    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
