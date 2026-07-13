// Standalone test for the llama.cpp tool-output parsers (llama-fit-params /
// llama-bench), driven entirely by synthetic captured outputs.

#include "lemon/backends/llamacpp/llamacpp_tools.h"

#include <cstdio>
#include <string>

using namespace lemon::backends::llamacpp;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static void test_parse_fit_params() {
    const std::string out =
        "load_backend: loaded RPC backend\n"
        "-c 32768 -ngl -1\n"
        "Vulkan0 3147 5760 301\n"
        "Host 304 0 50\n";
    FitEstimate f = parse_fit_params_output(out);
    check("fit: ok", f.ok);
    check("fit: ctx parsed", f.fitted_ctx == 32768);
    check("fit: full offload", f.fits_fully && f.fitted_ngl == -1);
    check("fit: device rows", f.devices.size() == 2 && f.devices[0].ctx_mib == 5760);
    check("fit: total excludes host", f.total_mib() == 3147 + 5760 + 301);

    FitEstimate bad = parse_fit_params_output("random log noise\n");
    check("fit: garbage rejected", !bad.ok && !bad.error.empty());

    FitEstimate moe = parse_fit_params_output("-c 16384 -ngl -1 -ncmoe 12\n");
    check("fit: ncmoe parsed, not full", moe.fitted_ncmoe == 12 && !moe.fits_fully);

    // Table-only output = nothing needed adjusting = full fit at requested ctx.
    FitEstimate table = parse_fit_params_output("Vulkan0 168 111 513\nHost 120 0 35\n");
    check("fit: table-only is a full fit", table.ok && table.fits_fully && table.fitted_ctx == 0);
}

static void test_parse_llama_bench() {
    const std::string out = R"(0.00.123 I llama_model_loader: loaded meta data
[
      {"n_prompt": 2048, "n_gen": 0, "n_depth": 0, "n_batch": 2048, "n_ubatch": 512, "avg_ts": 611.2},
      {"n_prompt": 0, "n_gen": 32, "n_depth": 0, "n_batch": 2048, "n_ubatch": 512, "avg_ts": 41.8},
      {"n_prompt": 2048, "n_gen": 0, "n_depth": 30000, "n_batch": 2048, "n_ubatch": 512, "avg_ts": 213.0},
      {"n_prompt": 0, "n_gen": 32, "n_depth": 30000, "n_batch": 2048, "n_ubatch": 512, "avg_ts": 28.4}
    ])";
    auto pts = parse_llama_bench_json(out, "vulkan");
    check("bench: two depth points", pts.size() == 2);
    check("bench: d0 pp+tg", pts[0].ok && pts[0].pp_avg_ts == 611.2 && pts[0].tg_avg_ts == 41.8);
    check("bench: d30000 keyed", pts[1].n_depth == 30000 && pts[1].tg_avg_ts == 28.4);

    auto bad = parse_llama_bench_json("not json", "vulkan");
    check("bench: parse error surfaces", bad.size() == 1 && !bad[0].ok && !bad[0].error.empty());
}

int main() {
    test_parse_fit_params();
    test_parse_llama_bench();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
