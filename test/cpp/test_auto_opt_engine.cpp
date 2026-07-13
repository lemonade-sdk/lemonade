// Standalone test for the AutoOpt decision engine: probe-output parsers and
// the 12-lever preset synthesis, driven entirely by synthetic inputs.

#include "lemon/auto_opt_engine.h"

#include <cstdio>
#include <string>

using namespace lemon::autoopt;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static bool has_arg(const GeneratedPreset& p, const std::string& needle) {
    return p.llamacpp_args.find(needle) != std::string::npos;
}

static HardwareSnapshot igpu_hw() {
    HardwareSnapshot hw;
    hw.gpus.push_back({"amd", "Radeon 8060S", "gfx1151", 96.0});
    hw.has_igpu = true;
    hw.ram_is_vram = true;
    hw.host_ram_gb = 128;
    hw.host_ram_available_gb = 80;
    hw.gpu_available_gb = 90;
    hw.installed_backends = {"vulkan", "rocm-stable"};
    hw.os = "linux";
    return hw;
}

static HardwareSnapshot dgpu_hw() {
    HardwareSnapshot hw;
    hw.gpus.push_back({"nvidia", "RTX 4090", "sm_89", 24.0});
    hw.host_ram_gb = 64;
    hw.host_ram_available_gb = 40;
    hw.gpu_available_gb = 22;
    hw.installed_backends = {"cuda"};
    hw.os = "linux";
    return hw;
}

static ModelFacts dense_model() {
    ModelFacts mf;
    mf.architecture = "qwen3";
    mf.block_count = 36;
    mf.n_ctx_train = 131072;
    mf.file_size_gb = 18;
    mf.kv_bytes_per_token = 90112;
    return mf;
}

static FitEstimate fitting(const std::string& backend, int ctx, const std::string& extra = "") {
    FitEstimate f;
    f.backend = backend;
    f.extra_args = extra;
    f.fitted_args = "-c " + std::to_string(ctx) + " -ngl -1";
    f.fitted_ctx = ctx;
    f.fitted_ngl = -1;
    f.devices.push_back({"Vulkan0", 17408, 6144, 910});
    f.fits_fully = true;
    f.ok = true;
    return f;
}

// ── parsers ────────────────────────────────────────────────────────────

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

// ── levers ─────────────────────────────────────────────────────────────

static void test_lever1_cache_ram() {
    WizardAnswers ans;
    ans.ram_headroom = "reduced";
    auto r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, {}, {});
    check("L1: reduced scale", has_arg(r.primary, "--cache-ram 4096 -ctxcp 16"));

    ans.ram_headroom = "normal";
    r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, {}, {});
    check("L1: normal omits flags", !has_arg(r.primary, "--cache-ram"));
}

static void test_lever1_hybrid_bump() {
    WizardAnswers ans;
    ans.ram_headroom = "disabled";
    ModelFacts mf = dense_model();
    mf.full_attention_interval = 4;
    mf.is_hybrid_or_recurrent = true;
    auto r = synthesize(igpu_hw(), mf, ans, {fitting("vulkan", 65536)}, {}, {});
    check("L1: hybrid never disabled", has_arg(r.primary, "--cache-ram 2048 -ctxcp 8"));
    check("L1: hybrid bump explained",
          [&] {
              for (const auto& s : r.primary.rationale)
                  if (s.find("hybrid/recurrent") != std::string::npos) return true;
              return false;
          }());
}

static void test_lever2_kv_quant() {
    WizardAnswers ans;
    ans.kv_cache_quant = "q8_0";
    auto r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 32768)}, {}, {});
    check("L2: ctk/ctv emitted", has_arg(r.primary, "-ctk q8_0 -ctv q8_0"));
    check("L2: ctx doubled by q8_0", r.primary.ctx_size == 65536);

    check("L2: max-quality alternative present",
          !r.alternatives.empty() && r.alternatives[0].label == "Maximum quality"
              && r.alternatives[0].llamacpp_args.find("-ctk") == std::string::npos);
    check("L2: max-context alternative q4_0",
          r.alternatives.size() >= 2
              && r.alternatives[1].llamacpp_args.find("-ctk q4_0") != std::string::npos);
}

static void test_lever3_split_mode() {
    WizardAnswers ans;
    HardwareSnapshot hw = dgpu_hw();
    hw.gpus.push_back({"nvidia", "RTX 4090", "sm_89", 24.0});
    auto r = synthesize(hw, dense_model(), ans, {fitting("cuda", 65536)}, {}, {});
    check("L3: tensor split on dual identical CUDA", has_arg(r.primary, "--split-mode tensor"));

    HardwareSnapshot vk = igpu_hw();
    vk.gpus.push_back({"amd", "RX 7900", "gfx1100", 24.0});
    r = synthesize(vk, dense_model(), ans, {fitting("vulkan", 65536)}, {}, {});
    check("L3: no tensor split off CUDA", !has_arg(r.primary, "--split-mode"));
}

static void test_lever4_parallel() {
    WizardAnswers ans;
    ans.parallel = true;
    ans.slots = 4;
    ans.dedicated_slots = true;
    auto r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, {}, {});
    check("L4: dedicated slots", has_arg(r.primary, "-np 4 -no-kvu"));
    check("L4: per-slot math in rationale",
          [&] {
              const std::string want = std::to_string(r.primary.ctx_size / 4);
              for (const auto& s : r.primary.rationale)
                  if (s.find(want) != std::string::npos) return true;
              return false;
          }());

    ans.dedicated_slots = false;
    r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, {}, {});
    check("L4: shared pool", has_arg(r.primary, "-np 4 -kvu"));
}

static void test_lever5_speculative() {
    WizardAnswers ans;
    auto r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, {}, {});
    check("L5: spec-default always", has_arg(r.primary, "--spec-default"));
    check("L5: no mtp flags on non-mtp", !has_arg(r.primary, "draft-mtp"));

    ModelFacts mtp = dense_model();
    mtp.has_mtp = true;
    std::vector<BenchPoint> sweep;
    for (int n = 1; n <= 4; ++n) {
        BenchPoint bp;
        bp.backend = "vulkan";
        bp.params = {{"spec_n", n}};
        bp.tg_avg_ts = n == 2 ? 55.0 : 40.0 + n;
        bp.ok = true;
        sweep.push_back(bp);
    }
    r = synthesize(igpu_hw(), mtp, ans, {fitting("vulkan", 65536)}, sweep, {});
    check("L5: measured argmax n=2", has_arg(r.primary, "--spec-draft-n-max 2"));

    r = synthesize(igpu_hw(), mtp, ans, {fitting("vulkan", 65536)}, {}, {});
    check("L5: default n=3 unmeasured", has_arg(r.primary, "--spec-draft-n-max 3"));
}

static void test_lever6_backend_duel() {
    WizardAnswers ans;
    std::vector<BenchPoint> duel;
    for (const std::string& b : {std::string("vulkan"), std::string("rocm-stable")}) {
        BenchPoint d0;
        d0.backend = b;
        d0.n_depth = 0;
        d0.params = {{"d", 0}};
        d0.pp_avg_ts = b == "vulkan" ? 600 : 580;
        d0.tg_avg_ts = b == "vulkan" ? 42 : 37;
        d0.ok = true;
        duel.push_back(d0);
    }
    auto r = synthesize(igpu_hw(), dense_model(), ans,
                        {fitting("vulkan", 65536), fitting("rocm-stable", 65536)}, duel, {});
    check("L6: measured winner", r.primary.llamacpp_backend == "vulkan");

    auto rh = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, {}, {});
    check("L6: heuristic fallback picks installed", rh.primary.llamacpp_backend == "vulkan");
}

static void test_lever7_sampling() {
    WizardAnswers ans;
    SamplingDefaults sd;
    sd.temperature = 0.7;
    sd.top_p = 0.8;
    sd.top_k = 20;
    sd.source = "hf:Qwen/Qwen3-32B/generation_config.json";
    auto r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, {}, sd);
    check("L7: sampling passthrough",
          r.sampling_defaults && r.sampling_defaults->temperature
              && *r.sampling_defaults->temperature == 0.7);
    check("L7: sampling not in args", !has_arg(r.primary, "temp"));
}

static void test_lever8_vision() {
    WizardAnswers ans;
    ans.use_vision = false;
    ModelFacts mf = dense_model();
    mf.has_vision = true;
    auto r = synthesize(igpu_hw(), mf, ans, {fitting("vulkan", 65536)}, {}, {});
    check("L8: mmproj disabled", !r.primary.mmproj_enabled);

    ans.use_vision = true;
    r = synthesize(igpu_hw(), mf, ans, {fitting("vulkan", 65536)}, {}, {});
    check("L8: mmproj kept", r.primary.mmproj_enabled);
}

static void test_lever9_batch_ladder() {
    WizardAnswers ans;
    std::vector<BenchPoint> ladder;
    for (int b : {512, 2048, 8192}) {
        BenchPoint bp;
        bp.backend = "vulkan";
        bp.n_depth = 0;
        bp.params = {{"d", 0}, {"ladder", true}, {"b", b}, {"ub", b}};
        bp.pp_avg_ts = b == 2048 ? 950 : (b == 8192 ? 900 : 600);
        bp.tg_avg_ts = 40;
        bp.ok = true;
        ladder.push_back(bp);
    }
    auto r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, ladder, {});
    check("L9: measured best rung", has_arg(r.primary, "-b 2048 -ub 2048"));

    auto rq = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, {}, {});
    check("L9: iGPU heuristic without bench", has_arg(rq.primary, "-b 2048 -ub 2048"));

    auto rd = synthesize(dgpu_hw(), dense_model(), ans, {fitting("cuda", 65536)}, {}, {});
    check("L9: no heuristic on dGPU", !has_arg(rd.primary, "-b "));
}

static void test_lever10_rocm_dio() {
    WizardAnswers ans;
    ans.backends_to_consider = {"rocm-stable"};
    auto r = synthesize(igpu_hw(), dense_model(), ans, {fitting("rocm-stable", 65536)}, {}, {});
    check("L10: rocm gets --direct-io", has_arg(r.primary, "--direct-io"));

    ans.backends_to_consider = {"vulkan"};
    r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 65536)}, {}, {});
    check("L10: vulkan does not", !has_arg(r.primary, "--direct-io"));
}

static void test_lever11_cpu_moe() {
    WizardAnswers ans;
    ModelFacts moe = dense_model();
    moe.is_moe = true;
    moe.expert_count = 128;
    FitEstimate f = fitting("vulkan", 32768);
    f.fitted_ngl = 20;
    f.fitted_ncmoe = 12;
    f.fits_fully = false;
    auto r = synthesize(igpu_hw(), moe, ans, {f}, {}, {});
    check("L11: n-cpu-moe from fit", has_arg(r.primary, "--n-cpu-moe 12"));
    check("L11: conservative alternative",
          [&] {
              for (const auto& a : r.alternatives)
                  if (a.llamacpp_args.find("--cpu-moe") != std::string::npos) return true;
              return false;
          }());
}

static void test_lever12_ctx_fit() {
    WizardAnswers ans;
    auto r = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 40000)}, {}, {});
    check("L12: ctx rounded down to fit", r.primary.ctx_size == 32768);

    auto rmax = synthesize(igpu_hw(), dense_model(), ans, {fitting("vulkan", 200000)}, {}, {});
    check("L12: ctx capped at trained window", rmax.primary.ctx_size == 131072);
}

int main() {
    test_parse_fit_params();
    test_parse_llama_bench();
    test_lever1_cache_ram();
    test_lever1_hybrid_bump();
    test_lever2_kv_quant();
    test_lever3_split_mode();
    test_lever4_parallel();
    test_lever5_speculative();
    test_lever6_backend_duel();
    test_lever7_sampling();
    test_lever8_vision();
    test_lever9_batch_ladder();
    test_lever10_rocm_dio();
    test_lever11_cpu_moe();
    test_lever12_ctx_fit();

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
