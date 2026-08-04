#pragma once

#include "lemon_cli/bench.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace lemon_cli {

using json = nlohmann::json;

struct FieldWidths {
    size_t scenario_name = 20;
    size_t ttft = 8;
    size_t tps = 8;
    size_t vram = 8;
};

// Compute percentiles for a sorted/unordered vector of doubles.
// Returns 0.0 for empty input.
double percentile(std::vector<double> values, double p);

// Formatting helpers (used internally and by comparison module)
std::string fmt_double(double val, int precision = 1);
std::string fmt_vram(double val);
std::string fmt_pct_change(double pct);
std::string fmt_vram_change(std::optional<double> val);

// Compute column widths for table output
FieldWidths calculate_field_widths(const std::vector<BenchBackendResult>& results);

// Print results as a formatted table to stdout.
// use_percentiles: show p50/p95 columns (true when runs >= 10); otherwise show min/max.
void print_table(const std::vector<BenchBackendResult>& results, const std::string& model,
                 bool use_percentiles);

// Convert benchmark results to JSON for programmatic consumption / file output.
json to_json(const std::vector<BenchBackendResult>& results,
             const std::string& model,
             const std::string& timestamp,
             const BenchConfig& config);

} // namespace lemon_cli
