// Pure AutoOpt decision engine: probe-output parsers and preset synthesis.
// No I/O and no server dependencies — everything arrives via the structs in
// auto_opt_types.h, so the standalone test binary needs zero linking.
#pragma once

#include "lemon/auto_opt_types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace lemon {
namespace autoopt {

// ── Probe-output parsers ───────────────────────────────────────────────

// llama-fit-params stdout: the first line starting with '-' carries the fitted
// args ("-c 32768 -ngl -1"); with -fitp on, subsequent "<dev> <model> <ctx>
// <compute>" rows follow (MiB). Anything else is log noise.
inline FitEstimate parse_fit_params_output(const std::string& output) {
    FitEstimate fit;
    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (fit.fitted_args.empty() && line[0] == '-') {
            fit.fitted_args = line;
            std::istringstream args(line);
            std::string tok;
            while (args >> tok) {
                if (tok == "-c" || tok == "--ctx-size") args >> fit.fitted_ctx;
                else if (tok == "-ngl" || tok == "--n-gpu-layers") args >> fit.fitted_ngl;
                else if (tok == "-ncmoe" || tok == "--n-cpu-moe") args >> fit.fitted_ncmoe;
            }
            continue;
        }
        FitEstimate::DeviceMem d;
        char dev[128] = {};
        if (std::sscanf(line.c_str(), "%127s %d %d %d", dev, &d.model_mib, &d.ctx_mib,
                        &d.compute_mib) == 4 && d.model_mib >= 0) {
            d.device = dev;
            fit.devices.push_back(d);
        }
    }
    // The tool prints the args line only when it had to ADJUST something; a
    // bare memory table means everything fits at the requested (or model
    // default, i.e. full trained) settings. fitted_ctx stays 0 = "no cap".
    fit.ok = !fit.fitted_args.empty() || !fit.devices.empty();
    fit.fits_fully = fit.ok && fit.fitted_ngl == -1 && fit.fitted_ncmoe == 0;
    if (!fit.ok) fit.error = "no fitted-args line or memory table in llama-fit-params output";
    return fit;
}

// llama-bench -o json: array of test objects. Rows with n_prompt>0/n_gen==0
// are prompt-processing, n_gen>0/n_prompt==0 are generation; keyed by n_depth.
// The captured stream interleaves stderr logs (subprocess pipes are merged),
// so the JSON array is extracted between the bare "[" and "]" lines first.
inline std::vector<BenchPoint> parse_llama_bench_json(const std::string& output,
                                                      const std::string& backend) {
    std::vector<BenchPoint> points;
    std::string json_text;
    {
        std::istringstream in(output);
        std::string line;
        bool inside = false;
        while (std::getline(in, line)) {
            std::string trimmed = line;
            while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == ' '))
                trimmed.pop_back();
            if (!inside && trimmed == "[") inside = true;
            if (inside) json_text += line + "\n";
            if (inside && trimmed == "]") break;
        }
        if (json_text.empty()) json_text = output;
    }
    json tests;
    try {
        tests = json::parse(json_text);
    } catch (const std::exception& e) {
        BenchPoint p;
        p.backend = backend;
        p.error = std::string("llama-bench JSON parse failed: ") + e.what();
        points.push_back(p);
        return points;
    }
    if (!tests.is_array()) return points;

    auto point_for_depth = [&points, &backend](int depth) -> BenchPoint& {
        for (auto& p : points)
            if (p.n_depth == depth) return p;
        BenchPoint p;
        p.backend = backend;
        p.n_depth = depth;
        p.params = {{"d", depth}};
        points.push_back(p);
        return points.back();
    };

    for (const auto& t : tests) {
        if (!t.is_object()) continue;
        const int depth = t.value("n_depth", 0);
        const int n_prompt = t.value("n_prompt", 0);
        const int n_gen = t.value("n_gen", 0);
        const double avg = t.value("avg_ts", 0.0);
        BenchPoint& p = point_for_depth(depth);
        if (n_prompt > 0 && n_gen == 0) p.pp_avg_ts = avg;
        else if (n_gen > 0 && n_prompt == 0) p.tg_avg_ts = avg;
        p.ok = p.pp_avg_ts > 0 || p.tg_avg_ts > 0;
    }
    return points;
}

// ── Synthesis helpers ──────────────────────────────────────────────────

namespace engine_detail {

inline const BenchPoint* find_bench(const std::vector<BenchPoint>& bench,
                                    const std::string& backend, int depth,
                                    const char* extra_key = nullptr, int extra_val = 0) {
    for (const auto& p : bench) {
        if (!p.ok || p.backend != backend || p.n_depth != depth) continue;
        if (extra_key) {
            if (!p.params.contains(extra_key) || p.params[extra_key].get<int>() != extra_val)
                continue;
        } else if (p.params.contains("spec_n") || p.params.contains("ladder")) {
            continue;
        }
        return &p;
    }
    return nullptr;
}

inline const FitEstimate* find_fit(const std::vector<FitEstimate>& fits,
                                   const std::string& backend,
                                   const std::string& extra_args = "") {
    for (const auto& f : fits)
        if (f.ok && f.backend == backend && f.extra_args == extra_args) return &f;
    return nullptr;
}

inline int round_down_ctx(int64_t ctx) {
    static const int steps[] = {262144, 131072, 98304, 65536, 49152, 32768,
                                24576, 16384, 12288, 8192, 6144, 4096, 2048};
    for (int s : steps)
        if (ctx >= s) return s;
    return ctx >= 1024 ? (int)ctx : 1024;
}

inline double kv_quant_factor(const std::string& q) {
    if (q == "q8_0") return 0.5;
    if (q == "q5_1") return 0.375;
    if (q == "q4_0") return 0.28125;
    return 1.0;
}

inline std::string pick_backend(const std::vector<BenchPoint>& bench,
                                const std::vector<std::string>& candidates,
                                const BenchPoint** deep_out = nullptr) {
    std::string best;
    double best_score = -1;
    for (const auto& c : candidates) {
        const BenchPoint* d0 = find_bench(bench, c, 0);
        if (!d0) continue;
        const BenchPoint* deep = nullptr;
        for (const auto& bp : bench)
            if (bp.ok && bp.backend == c && bp.n_depth > 0 && !bp.params.contains("ladder")
                && !bp.params.contains("spec_n"))
                deep = &bp;
        const double pp0 = d0->pp_avg_ts;
        const double tg = deep ? deep->tg_avg_ts : d0->tg_avg_ts;
        const double ppd = deep ? deep->pp_avg_ts : d0->pp_avg_ts;
        const double score = 0.35 * pp0 + 0.45 * tg * 10 + 0.20 * ppd;
        if (score > best_score) {
            best_score = score;
            best = c;
            if (deep_out) *deep_out = deep ? deep : d0;
        }
    }
    return best;
}

inline std::string fmt1(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    return buf;
}

}  // namespace engine_detail

// ── The 12-lever synthesis ─────────────────────────────────────────────

inline AutoOptResult synthesize(const HardwareSnapshot& hw, const ModelFacts& mf,
                                const WizardAnswers& ans,
                                const std::vector<FitEstimate>& fits,
                                const std::vector<BenchPoint>& bench,
                                const std::optional<SamplingDefaults>& sampling) {
    using namespace engine_detail;
    AutoOptResult result;
    GeneratedPreset& p = result.primary;
    p.label = "Recommended";
    std::vector<std::string> args;

    // Lever 6 (+3 constraint): backend selection — measured duel when bench
    // data exists, install-order heuristic otherwise.
    std::vector<std::string> candidates;
    for (const auto& b : hw.installed_backends) {
        if (b == "cpu" || b == "system") continue;
        if (!ans.backends_to_consider.empty()
            && std::find(ans.backends_to_consider.begin(), ans.backends_to_consider.end(), b)
                   == ans.backends_to_consider.end())
            continue;
        candidates.push_back(b);
    }
    if (candidates.empty()) candidates.push_back("vulkan");

    std::string backend = candidates.front();
    if (candidates.size() > 1) {
        const BenchPoint* best_deep = nullptr;
        const std::string measured = pick_backend(bench, candidates, &best_deep);
        if (!measured.empty()) backend = measured;
        if (best_deep) {
            std::string others;
            for (const auto& c : candidates)
                if (c != backend) others += (others.empty() ? "" : ", ") + c;
            p.rationale.push_back(backend + " chosen over " + others
                                  + ": best measured decode/prefill balance on this model ("
                                  + fmt1(best_deep->tg_avg_ts) + " tok/s decode at depth "
                                  + std::to_string(best_deep->n_depth) + ").");
        } else {
            p.rationale.push_back(backend + " chosen (first installed GPU backend; enable a "
                                            "benchmark budget to measure alternatives).");
        }
    }
    p.llamacpp_backend = backend;
    const bool is_rocm = backend.rfind("rocm", 0) == 0;
    const bool is_cuda = backend == "cuda";

    // Lever 10: mmap is broken on ROCm; direct I/O is the faster workaround.
    // (LlamaCppServer::load now defaults this for ROCm; kept in the preset so
    // exported args stay self-contained.)
    if (is_rocm) {
        args.push_back("--direct-io");
        p.rationale.push_back("--direct-io: works around broken mmap on ROCm with faster "
                              "cold loads than --no-mmap.");
    }

    // Lever 8: vision projector.
    if (mf.has_vision) {
        if (ans.use_vision && !*ans.use_vision) {
            p.mmproj_enabled = false;
            int freed = 0;
            const FitEstimate* with_mm = find_fit(fits, backend);
            const FitEstimate* without_mm = find_fit(fits, backend, "--no-mmproj");
            if (with_mm && without_mm) freed = with_mm->total_mib() - without_mm->total_mib();
            p.rationale.push_back(std::string("Vision projector disabled per your answer")
                                  + (freed > 0 ? " — frees ~" + std::to_string(freed)
                                                     + " MiB for context."
                                               : " — its memory goes to context instead."));
        } else {
            p.rationale.push_back("Vision projector kept loaded (image input enabled).");
        }
    }

    // Lever 11: fit strategy for models that don't fully fit.
    const FitEstimate* fit = find_fit(fits, backend);
    bool used_cpu_moe = false;
    if (fit && !fit->fits_fully) {
        if (mf.is_moe && fit->fitted_ncmoe > 0) {
            args.push_back("--n-cpu-moe " + std::to_string(fit->fitted_ncmoe));
            used_cpu_moe = true;
            p.rationale.push_back("Model exceeds GPU memory: expert tensors of the first "
                                  + std::to_string(fit->fitted_ncmoe)
                                  + " layers stay on CPU (--n-cpu-moe) — attention and shared "
                                    "tensors keep GPU speed.");
        } else if (mf.is_moe) {
            args.push_back("--cpu-moe");
            used_cpu_moe = true;
            p.rationale.push_back("Model exceeds GPU memory: all expert tensors on CPU "
                                  "(--cpu-moe); the GPU keeps the non-expert layers.");
        } else if (fit->fitted_ngl >= 0) {
            p.rationale.push_back("Model exceeds GPU memory: llama.cpp will offload "
                                  + std::to_string(fit->fitted_ngl)
                                  + " layers to GPU and run the rest on CPU (expect reduced "
                                    "speed).");
        }
    }

    // Lever 2 + ctx: KV-cache quantization (user pick is a constraint) and the
    // largest context that fits under it.
    const std::string& kv = ans.kv_cache_quant;
    if (kv != "none") {
        args.push_back("-ctk " + kv + " -ctv " + kv);
        const char* note = kv == "q8_0"
            ? "roughly doubles usable context with negligible quality loss"
            : (kv == "q5_1" ? "about 2.7x context capacity with a slight quality cost"
                            : "about 3.5x context capacity; quality measurably degrades on "
                              "very long contexts");
        p.rationale.push_back("KV cache quantized to " + kv + ": " + std::string(note) + ".");
    }
    int64_t ctx = mf.n_ctx_train > 0 ? mf.n_ctx_train : 32768;
    if (fit && fit->fitted_ctx > 0) {
        int64_t fit_ctx = fit->fitted_ctx;
        if (kv != "none")
            fit_ctx = (int64_t)((double)fit_ctx / kv_quant_factor(kv));
        ctx = std::min<int64_t>(ctx, fit_ctx);
    }
    p.ctx_size = round_down_ctx(ctx);
    if (mf.n_ctx_train > 0 && p.ctx_size >= mf.n_ctx_train) {
        p.ctx_size = (int)mf.n_ctx_train;
        p.rationale.push_back("Context " + std::to_string(p.ctx_size)
                              + ": the model's full trained window fits.");
    } else {
        p.rationale.push_back("Context " + std::to_string(p.ctx_size)
                              + ": the largest standard size that fits in memory"
                              + (kv != "none" ? " with the quantized KV cache." : "."));
    }

    // Lever 1: prompt-cache checkpoints scale (user pick constrained by the
    // hybrid/recurrent bump).
    std::string headroom = ans.ram_headroom;
    if (mf.is_hybrid_or_recurrent && headroom == "disabled") {
        headroom = "minimal";
        p.rationale.push_back("Prompt-cache checkpoints kept at minimal instead of disabled: "
                              "this architecture (hybrid/recurrent) must re-process the whole "
                              "prompt on any cache miss.");
    }
    if (headroom == "reduced") args.push_back("--cache-ram 4096 -ctxcp 16");
    else if (headroom == "minimal") args.push_back("--cache-ram 2048 -ctxcp 8");
    else if (headroom == "disabled") args.push_back("--cache-ram 0 -ctxcp 0");
    if (headroom != "normal") {
        p.rationale.push_back("Prompt-cache RAM capped (" + headroom + ")"
                              + (hw.ram_is_vram ? " — on this machine system RAM and GPU "
                                                  "memory share one pool."
                                                : " to keep system RAM free."));
    }

    // Lever 4: parallel slots.
    if (ans.parallel && ans.slots > 1) {
        const int np = std::min(std::max(ans.slots, 2), 8);
        if (ans.dedicated_slots) {
            args.push_back("-np " + std::to_string(np) + " -no-kvu");
            p.rationale.push_back(std::to_string(np) + " parallel slots with dedicated "
                                  "context: each request is guaranteed "
                                  + std::to_string(p.ctx_size / np) + " tokens ("
                                  + std::to_string(p.ctx_size) + " / " + std::to_string(np)
                                  + ").");
        } else {
            args.push_back("-np " + std::to_string(np) + " -kvu");
            p.rationale.push_back(std::to_string(np) + " parallel slots sharing one context "
                                  "pool: long requests can use most of the window, but "
                                  "concurrent long requests race for it.");
        }
    }

    // Lever 5: speculative decoding.
    args.push_back("--spec-default");
    p.rationale.push_back("--spec-default: n-gram speculative decoding is effectively free.");
    if (mf.has_mtp) {
        int best_n = 3;
        double best_ts = -1;
        for (const auto& bp : bench) {
            if (!bp.ok || !bp.params.contains("spec_n")) continue;
            if (bp.tg_avg_ts > best_ts) {
                best_ts = bp.tg_avg_ts;
                best_n = bp.params["spec_n"].get<int>();
            }
        }
        args.push_back("--spec-type draft-mtp --spec-draft-n-max " + std::to_string(best_n));
        p.rationale.push_back(best_ts > 0
            ? "MTP draft length " + std::to_string(best_n) + " measured fastest on this "
              "machine (" + fmt1(best_ts) + " tok/s)."
            : "Model has MTP heads: draft-based speculative decoding enabled (default "
              "draft length 3).");
    }

    // Lever 9: batch/ubatch ladder for unified-memory boxes with RAM to spare.
    const bool big_igpu = hw.ram_is_vram && hw.host_ram_gb >= 32;
    {
        int best_b = 0;
        double best_pp = -1, base_pp = -1;
        for (const auto& bp : bench) {
            if (!bp.ok || !bp.params.contains("ladder") || bp.backend != backend
                || bp.n_depth != 0)
                continue;
            const int b = bp.params["b"].get<int>();
            if (b == 512) base_pp = bp.pp_avg_ts;
            if (bp.pp_avg_ts > best_pp) {
                best_pp = bp.pp_avg_ts;
                best_b = b;
            }
        }
        if (best_b > 512 && base_pp > 0 && best_pp > base_pp * 1.05) {
            args.push_back("-b " + std::to_string(best_b) + " -ub " + std::to_string(best_b));
            p.rationale.push_back("Batch size " + std::to_string(best_b) + " measured "
                                  + fmt1((best_pp / base_pp - 1) * 100)
                                  + "% faster prefill than the default 512 (costs some "
                                    "memory for compute buffers).");
        } else if (bench.empty() && big_igpu) {
            args.push_back("-b 2048 -ub 2048");
            p.rationale.push_back("Batch size 2048: unified-memory machines with ample RAM "
                                  "prefill much faster with larger batches (heuristic — run "
                                  "a standard/thorough pass to measure).");
        }
    }

    // Lever 3: tensor parallelism across identical GPUs (CUDA-only today).
    if (is_cuda) {
        int same_family = 0;
        for (size_t i = 0; i < hw.gpus.size(); ++i)
            for (size_t j = i + 1; j < hw.gpus.size(); ++j)
                if (hw.gpus[i].vendor == "nvidia" && hw.gpus[i].family == hw.gpus[j].family)
                    same_family++;
        if (same_family > 0) {
            args.push_back("--split-mode tensor");
            p.rationale.push_back("Two or more identical NVIDIA GPUs: tensor parallelism "
                                  "speeds up decode (prefill gets slightly slower).");
        }
    }

    // Lever 7: sampling defaults pass through (request-time, not load flags).
    result.sampling_defaults = sampling;
    if (sampling)
        p.rationale.push_back("Sampling defaults (temperature/top-p/top-k) taken from the "
                              "base model's generation_config (" + sampling->source + ").");

    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& a : v) s += (s.empty() ? "" : " ") + a;
        return s;
    };
    p.llamacpp_args = join(args);
    if (fit) {
        const BenchPoint* d0 = find_bench(bench, backend, 0);
        p.expected = json::object();
        p.expected["vram_mib"] = fit->total_mib();
        if (d0) {
            p.expected["pp_ts"] = d0->pp_avg_ts;
            p.expected["tg_ts"] = d0->tg_avg_ts;
        }
    }

    // ── Alternatives ───────────────────────────────────────────────────
    if (kv != "none") {
        GeneratedPreset alt = p;
        alt.label = "Maximum quality";
        alt.tradeoff = "smaller context window";
        alt.rationale = {"Unquantized f16 KV cache: no quality risk; context shrinks to what "
                         "fits."};
        std::vector<std::string> a2;
        for (const auto& a : args)
            if (a.rfind("-ctk", 0) != 0) a2.push_back(a);
        alt.llamacpp_args = join(a2);
        int64_t alt_ctx = (int64_t)(p.ctx_size * kv_quant_factor(kv));
        alt.ctx_size = round_down_ctx(std::max<int64_t>(alt_ctx, 4096));
        alt.expected = json::object();
        result.alternatives.push_back(alt);
    }
    if (kv != "q4_0") {
        GeneratedPreset alt = p;
        alt.label = "Maximum context";
        alt.tradeoff = "quality degrades on very long contexts";
        alt.rationale = {"q4_0 KV cache: about 3.5x the context capacity of f16; noticeable "
                         "quality loss past ~32k tokens of active context."};
        std::vector<std::string> a2;
        bool replaced = false;
        for (const auto& a : args) {
            if (a.rfind("-ctk", 0) == 0) {
                a2.push_back("-ctk q4_0 -ctv q4_0");
                replaced = true;
            } else {
                a2.push_back(a);
            }
        }
        if (!replaced) a2.insert(a2.begin(), "-ctk q4_0 -ctv q4_0");
        alt.llamacpp_args = join(a2);
        int64_t alt_ctx = (int64_t)((double)p.ctx_size * kv_quant_factor(kv) / kv_quant_factor("q4_0"));
        alt.ctx_size = round_down_ctx(std::min<int64_t>(
            alt_ctx, mf.n_ctx_train > 0 ? mf.n_ctx_train : alt_ctx));
        alt.expected = json::object();
        result.alternatives.push_back(alt);
    }
    if (used_cpu_moe) {
        GeneratedPreset alt = p;
        alt.label = "Conservative offload";
        alt.tradeoff = "slower, but immune to memory pressure";
        alt.rationale = {"All expert tensors on CPU (--cpu-moe): the safest fit when other "
                         "applications compete for GPU memory."};
        std::vector<std::string> a2;
        for (const auto& a : args) {
            if (a.rfind("--n-cpu-moe", 0) == 0) a2.push_back("--cpu-moe");
            else a2.push_back(a);
        }
        alt.llamacpp_args = join(a2);
        alt.expected = json::object();
        result.alternatives.push_back(alt);
    }

    return result;
}

}  // namespace autoopt
}  // namespace lemon
