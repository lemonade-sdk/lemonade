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

static void test_validate_fit_params_request() {
    auto ok = [](const char* body) { return validate_fit_params_request(json::parse(body)).empty(); };
    check("vfit: minimal ok", ok(R"({"model":"m","backend":"vulkan"})"));
    check("vfit: args string ok", ok(R"({"model":"m","backend":"vulkan","args":"-c 4096"})"));
    check("vfit: args array ok", ok(R"({"model":"m","backend":"vulkan","args":["-c","4096"]})"));
    check("vfit: fit_target ok", ok(R"({"model":"m","backend":"vulkan","fit_target_mib":2048})"));
    check("vfit: missing model rejected", !ok(R"({"backend":"vulkan"})"));
    check("vfit: empty model rejected", !ok(R"({"model":"","backend":"vulkan"})"));
    check("vfit: missing backend rejected", !ok(R"({"model":"m"})"));
    check("vfit: backend path chars rejected", !ok(R"({"model":"m","backend":"../evil"})"));
    check("vfit: unknown key rejected", !ok(R"({"model":"m","backend":"vulkan","bogus":1})"));
    check("vfit: args number rejected", !ok(R"({"model":"m","backend":"vulkan","args":42})"));
    check("vfit: args array of numbers rejected",
          !ok(R"({"model":"m","backend":"vulkan","args":[1,2]})"));
    check("vfit: fit_target negative rejected",
          !ok(R"({"model":"m","backend":"vulkan","fit_target_mib":-1})"));
    check("vfit: fit_target float rejected",
          !ok(R"({"model":"m","backend":"vulkan","fit_target_mib":1.5})"));
    check("vfit: non-object rejected", !validate_fit_params_request(json::parse("[1]")).empty());
    check("vfit: error names field",
          validate_fit_params_request(json::parse(R"({"model":"m","backend":"vulkan","x":1})"))
                  .find("'x'") != std::string::npos);
}

static void test_validate_bench_request() {
    auto ok = [](const char* body) { return validate_bench_request(json::parse(body)).empty(); };
    check("vbench: minimal ok", ok(R"({"model":"m","backend":"vulkan"})"));
    check("vbench: d int ok", ok(R"({"model":"m","backend":"vulkan","d":0})"));
    check("vbench: d array ok", ok(R"({"model":"m","backend":"vulkan","d":[0,30000]})"));
    check("vbench: full ok",
          ok(R"({"model":"m","backend":"vulkan","d":[0],"b":2048,"ub":2048,"ctk":"q8_0","ctv":"q8_0"})"));
    check("vbench: unknown key rejected", !ok(R"({"model":"m","backend":"vulkan","depth":0})"));
    check("vbench: d negative rejected", !ok(R"({"model":"m","backend":"vulkan","d":-1})"));
    check("vbench: d string rejected", !ok(R"({"model":"m","backend":"vulkan","d":"0"})"));
    check("vbench: d empty array rejected", !ok(R"({"model":"m","backend":"vulkan","d":[]})"));
    check("vbench: d mixed array rejected", !ok(R"({"model":"m","backend":"vulkan","d":[0,"x"]})"));
    check("vbench: b zero rejected", !ok(R"({"model":"m","backend":"vulkan","b":0})"));
    check("vbench: b huge rejected", !ok(R"({"model":"m","backend":"vulkan","b":100000})"));
    check("vbench: ub without b rejected", !ok(R"({"model":"m","backend":"vulkan","ub":512})"));
    check("vbench: ctk bogus rejected", !ok(R"({"model":"m","backend":"vulkan","ctk":"q9_9"})"));
    check("vbench: ctk non-string rejected", !ok(R"({"model":"m","backend":"vulkan","ctk":8})"));
    check("vbench: ctv without ctk rejected", !ok(R"({"model":"m","backend":"vulkan","ctv":"q8_0"})"));
    check("vbench: all cache types accepted", [&] {
        for (const char* t : {"f32", "f16", "bf16", "q8_0", "q5_1", "q5_0", "q4_1", "q4_0", "iq4_nl"}) {
            json b = {{"model", "m"}, {"backend", "vulkan"}, {"ctk", t}, {"ctv", t}};
            if (!validate_bench_request(b).empty()) return false;
        }
        return true;
    }());
}

int main() {
    test_parse_fit_params();
    test_parse_llama_bench();
    test_validate_fit_params_request();
    test_validate_bench_request();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}
