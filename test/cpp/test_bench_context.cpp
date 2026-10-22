#include <lemon_cli/bench_context.h>

#include <cstdio>
#include <vector>

using lemon_cli::bench_context_for_target_tokens;
using lemon_cli::plan_bench_contexts;

static int failed = 0;

static void check(bool ok, const char* message) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failed;
}

int main() {
    check(bench_context_for_target_tokens(0) == 0, "no target: no minimum");
    check(bench_context_for_target_tokens(4096) == 4096, "context-4k stays 4k");
    check(bench_context_for_target_tokens(32000) == 32768, "context-32k gets 32k");
    check(bench_context_for_target_tokens(64000) == 65536, "context-64k gets 64k");
    check(bench_context_for_target_tokens(128000) == 131072, "context-128k gets 128k");
    check(bench_context_for_target_tokens(5000) == 8192, "custom target is rounded up");

    const std::vector<int> required = {0, 4096, 32768, 65536, 131072};
    const auto defaults = plan_bench_contexts({}, 4096, required);
    check(defaults.skipped.empty() && defaults.batches.size() == 4,
          "default sizes grouped into four distinct contexts");
    check(defaults.batches.at(4096) == std::vector<std::size_t>({0, 1}),
          "regular and 4k scenarios share 4k");
    check(defaults.batches.at(32768) == std::vector<std::size_t>({2}),
          "32k scenario is only run at 32k");
    check(defaults.batches.at(65536) == std::vector<std::size_t>({3}),
          "64k scenario is only run at 64k");
    check(defaults.batches.at(131072) == std::vector<std::size_t>({4}),
          "128k scenario is only run at 128k");

    const auto model_override = plan_bench_contexts({}, 65536, required);
    check(model_override.batches.size() == 2 &&
          model_override.batches.at(65536) == std::vector<std::size_t>({0, 1, 2, 3}) &&
          model_override.batches.at(131072) == std::vector<std::size_t>({4}),
          "saved positive model context is retained as a floor");
    const auto auto_saved = plan_bench_contexts({}, -1, {0, 32768});
    check(auto_saved.batches.count(-1) == 1 && auto_saved.batches.count(32768) == 1,
          "saved server-auto is preserved for ordinary scenarios");

    const auto explicit_sweep = plan_bench_contexts({4096, 32768}, 4096, required);
    check(explicit_sweep.batches.size() == 2 &&
          explicit_sweep.batches.at(4096) == std::vector<std::size_t>({0, 1}) &&
          explicit_sweep.batches.at(32768) == std::vector<std::size_t>({0, 1, 2}),
          "explicit sweep only includes scenarios that fit each context");
    check(explicit_sweep.skipped.size() == 5,
          "explicit undersized context skips once per incompatible scenario");
    const auto too_small = plan_bench_contexts({4096}, 4096, {32768});
    check(too_small.batches.empty() && too_small.skipped.size() == 1,
          "undersized explicit-only request fails preflight");
    const auto server_auto = plan_bench_contexts({-1}, 4096, {32768});
    check(server_auto.batches.count(-1) == 1 && server_auto.skipped.empty(),
          "explicit nonpositive context delegates to server defaults");

    std::printf("bench context regression: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
