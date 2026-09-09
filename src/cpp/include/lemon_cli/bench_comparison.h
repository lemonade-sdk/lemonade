#pragma once

#include "lemon_cli/bench.h"
#include "lemon_cli/bench_output.h"
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace lemon_cli {

using json = nlohmann::json;

struct BenchComparisonDelta {
    std::string backend;
    int ctx_size = 0;
    std::string backend_args;
    std::string scenario;
    double ttft_pct_change;    // Positive = slower, negative = faster
    double tps_pct_change;     // Positive = faster, negative = slower
    std::optional<double> vram_gb_change; // Positive = more VRAM used, nullopt = no data
    std::string status;        // "matched", "new", "removed", "failed", "prev_failed"
};

// Load previous results from a JSON file. Returns empty object on error.
json load_previous_results(const std::string& file_path);

// Normalize benchmark JSON into a model->result-object map.
// Supports both legacy single-model files and multi-model files.
std::map<std::string, json> extract_models_from_results(const json& root);

// Extract the top-level timestamp from benchmark JSON.
std::string extract_results_timestamp(const json& root);

// Join a vector of strings with ", ". Returns "-" for empty input.
std::string join_list(const std::vector<std::string>& values);

// Compute deltas between current and previous benchmark results.
std::vector<BenchComparisonDelta> compute_deltas(const std::vector<BenchBackendResult>& current,
                                                  const json& previous_results);

// Print comparison table to stdout.
void print_comparison(const std::vector<BenchComparisonDelta>& deltas,
                      const std::string& model,
                      const std::string& previous_file,
                      const std::string& previous_timestamp);

// Build comparison JSON (for --json --compare).
json build_comparison_json(const std::vector<BenchBackendResult>& results,
                           const std::string& model,
                           const std::string& timestamp,
                           const BenchConfig& config,
                           const json& model_info,
                           const json& previous_results,
                           const std::vector<BenchComparisonDelta>& deltas);

} // namespace lemon_cli
