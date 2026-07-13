// Measurement providers for AutoOpt: llama-fit-params / llama-bench / a
// short-lived llama-server decode probe / HF generation_config fetch. The
// interface seam exists so the run manager can be tested without subprocesses.
#pragma once

#include "lemon/auto_opt_types.h"

#include <atomic>
#include <functional>

namespace lemon {
namespace autoopt {

using CancelFlag = std::atomic<bool>;
using ProgressFn = std::function<void(const std::string& detail)>;

class MeasurementProvider {
public:
    virtual ~MeasurementProvider() = default;

    virtual FitEstimate fit_params(const std::string& backend, const std::string& gguf_path,
                                   const std::vector<std::string>& extra_args,
                                   int fit_target_mib, CancelFlag& cancel) = 0;

    // One llama-bench invocation; `params` may carry {"d": [depths]}, {"b": N, "ub": N},
    // {"ctk": "q8_0"}. Returns one BenchPoint per depth.
    virtual std::vector<BenchPoint> llama_bench(const std::string& backend,
                                                const std::string& gguf_path, const json& params,
                                                CancelFlag& cancel, ProgressFn progress) = 0;

    // Spawns llama-server with `args`, runs a fixed completion twice, returns
    // the second run's decode tok/s (timings.predicted_per_second).
    virtual std::optional<double> server_decode_probe(const std::string& backend,
                                                      const std::string& gguf_path,
                                                      const std::vector<std::string>& args,
                                                      CancelFlag& cancel) = 0;

    // base-model repo -> generation_config.json (soft-fail: nullopt).
    virtual std::optional<json> fetch_generation_config(const std::string& repo) = 0;
};

class RealMeasurementProvider : public MeasurementProvider {
public:
    FitEstimate fit_params(const std::string& backend, const std::string& gguf_path,
                           const std::vector<std::string>& extra_args, int fit_target_mib,
                           CancelFlag& cancel) override;
    std::vector<BenchPoint> llama_bench(const std::string& backend, const std::string& gguf_path,
                                        const json& params, CancelFlag& cancel,
                                        ProgressFn progress) override;
    std::optional<double> server_decode_probe(const std::string& backend,
                                              const std::string& gguf_path,
                                              const std::vector<std::string>& args,
                                              CancelFlag& cancel) override;
    std::optional<json> fetch_generation_config(const std::string& repo) override;

private:
    std::string tool_path(const std::string& backend, const std::string& tool) const;
    std::vector<std::pair<std::string, std::string>> tool_env(const std::string& backend,
                                                              const std::string& exe) const;
};

// Resolve the HF repo id of the non-quantized base model: GGUF provenance
// first, then the HF API's cardData.base_model. Empty when unresolvable.
std::string resolve_base_model_repo(const std::string& gguf_base_model_repo,
                                    const std::string& checkpoint);

}  // namespace autoopt
}  // namespace lemon
