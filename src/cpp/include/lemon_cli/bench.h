#ifndef LEMON_CLI_BENCH_H
#define LEMON_CLI_BENCH_H

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace lemonade {
    class LemonadeClient;
}

namespace CLI { class App; }

namespace lemon_cli {

using json = nlohmann::json;

// ============================================================
// Data Types
// ============================================================

struct BenchScenario {
    std::string name;
    std::string category;
    std::vector<json> messages;  // Chat messages (system + user/assistant turns)
    int max_tokens;
    int warmup_runs = 0;
    int measurement_runs = 3;
};

struct BenchRunResult {
    double ttft_ms = 0.0;
    double tps = 0.0;
    int input_tokens = 0;
    int output_tokens = 0;
    double total_time_ms = 0.0;
    double vram_gb = -1.0;      // -1 means not available
    double memory_gb = -1.0;    // -1 means not available
    bool success = true;        // false if the run failed (exception, HTTP error, etc.)
};

struct BenchScenarioResult {
    std::string scenario_name;
    std::string category;
    std::vector<BenchRunResult> runs;
    int failed_runs = 0;        // number of runs that failed and were excluded from stats

    double ttft_mean_ms() const;
    double ttft_min_ms() const;
    double ttft_max_ms() const;
    double ttft_p50_ms() const;
    double ttft_p95_ms() const;
    double tps_mean() const;
    double tps_min() const;
    double tps_max() const;
    double tps_p50() const;
    double tps_p95() const;
    double vram_peak_gb() const;
    double memory_peak_gb() const;
    int input_tokens() const;    // From first run
    int output_tokens() const;   // From first run
};

struct BenchBackendResult {
    std::string recipe;         // e.g., "llamacpp"
    std::string backend;        // e.g., "vulkan", "metal", "cpu"
    int ctx_size = 0;
    std::string backend_args;   // Custom args passed to the backend (e.g., "--threads 8")
    std::vector<BenchScenarioResult> scenarios;

    // Human-readable label: "recipe/backend (ctx=N) args=[...]"
    std::string label() const;
};

// ============================================================
// Scenario Loading
// ============================================================

// Load scenarios from a single JSON file
std::vector<BenchScenario> load_scenarios_from_file(const std::string& path);

// Load all .json scenario files from a directory, merge scenarios
std::vector<BenchScenario> load_scenarios_from_dir(const std::string& path);

// Resolve the bundled default scenario file path
std::string resolve_default_scenario_file();

// Filter scenarios by name, category, or "all" (empty = return all).
// Each token matches if it equals a scenario's name, its category, or is "all".
std::vector<BenchScenario> filter_scenarios(const std::vector<BenchScenario>& all,
                                            const std::vector<std::string>& tokens);

// Expand context filler text to target token count
std::string expand_context(const json& context_block, const std::vector<json>& messages);

// ============================================================
// Backend Discovery
// ============================================================

struct BackendDiscovery {
    std::string recipe;
    std::string backend;
};

// Discover available backends for a model
std::vector<BackendDiscovery> discover_backends(lemonade::LemonadeClient& client,
                                                const std::string& model,
                                                const std::vector<std::string>& requested);

// ============================================================
// Model Load/Unload
// ============================================================

// Load model for a specific backend
bool load_model_for_backend(lemonade::LemonadeClient& client,
                            const std::string& model,
                            const std::string& recipe,
                            const std::string& backend,
                            int ctx_size,
                            const std::string& backend_args);

// Unload all models
bool unload_all_models(lemonade::LemonadeClient& client);

// ============================================================
// Benchmark Execution
// ============================================================

// Run a single benchmark measurement
BenchRunResult run_single_bench(lemonade::LemonadeClient& client,
                                const std::string& model,
                                const BenchScenario& scenario,
                                bool memory_tracking);

// Run a full scenario (warmup + measurement runs).
// When reload=true, unloads+loads the model before each measurement run to clear prompt cache.
BenchScenarioResult run_scenario(lemonade::LemonadeClient& client,
                                 const std::string& model,
                                 const BenchScenario& scenario,
                                 int warmup,
                                 int runs,
                                 bool memory_tracking,
                                 bool reload,
                                 const std::string& recipe,
                                 const std::string& backend,
                                 int ctx_size,
                                 const std::string& backend_args);

// ============================================================
// CLI Options (raw values parsed by CLI11 in main.cpp)
// ============================================================

struct BenchCliOptions {
    std::vector<std::string> models;
    std::vector<std::string> backends;
    std::vector<int> ctx_sizes;
    int runs = 3;
    int warmup = 0;
    std::vector<std::string> scenario_names;
    std::string scenario_file;
    std::string scenario_dir;
    bool json_output = false;
    bool auto_pull = false;
    bool no_memory = false;
    bool no_reload = false;  // disable reload between runs
    std::string compare_file;
    // Backend-specific custom args (repeatable for multiple comparisons)
    std::vector<std::string> llamacpp_args;
    std::vector<std::string> flm_args;
    std::vector<std::string> vllm_args;
    std::vector<std::string> sdcpp_args;
    std::vector<std::string> whispercpp_args;
};

// ============================================================
// Main Bench Handler
// ============================================================

struct BenchConfig {
    std::vector<std::string> models;
    std::vector<std::string> backends;
    std::vector<int> ctx_sizes;
    int warmup_runs = 0;
    int measurement_runs = 3;
    bool json_output = false;
    std::string output_file;
    std::vector<std::string> scenario_names;
    std::string scenario_file;
    std::string scenario_dir;
    bool auto_pull = false;
    bool memory_tracking = true;
    bool reload = true;  // unload+reload model between runs to clear prompt cache
    std::string compare_file;
    // Backend-specific custom args (keyed by recipe name: "llamacpp", "flm", "vllm", "sd-cpp", "whispercpp")
    // Each recipe can have multiple arg sets; all combinations are benchmarked.
    std::map<std::string, std::vector<std::string>> backend_args;
};

// Main entry point for bench command
int handle_bench_command(lemonade::LemonadeClient& client, const BenchConfig& config);

// Build BenchConfig from raw CLI options
BenchConfig build_bench_config(
                               const std::string& output_file,
                               const BenchCliOptions& cli);

// Register all bench subcommand options with CLI11.
// Returns the created subcommand pointer.
CLI::App* register_bench_command(CLI::App& parent,
                                 std::string& output_file,
                                 BenchCliOptions& opts);

// ============================================================
// Output Formatting
// ============================================================

// Print results as a comparison table to stdout
// use_percentiles: show p50/p95 columns (true when runs >= 10); otherwise show min/max
void print_table(const std::vector<BenchBackendResult>& results, const std::string& model,
                 bool use_percentiles);

// Convert results to JSON for programmatic consumption
json to_json(const std::vector<BenchBackendResult>& results,
             const std::string& model,
             const std::string& timestamp,
             const BenchConfig& config);

// ============================================================
// Comparison
// ============================================================

struct BenchComparisonDelta {
    std::string backend;
    int ctx_size = 0;
    std::string backend_args;
    std::string scenario;
    double ttft_pct_change;    // Positive = slower, negative = faster
    double tps_pct_change;     // Positive = faster, negative = slower
    std::optional<double> vram_gb_change; // Positive = more VRAM used, nullopt = no data
    std::string status;        // "matched", "new", "removed"
};

// Load previous results from a JSON file
json load_previous_results(const std::string& file_path);

// Compute deltas between current and previous results
std::vector<BenchComparisonDelta> compute_deltas(const std::vector<BenchBackendResult>& current,
                                                  const json& previous_results);

// Print comparison table to stdout
void print_comparison(const std::vector<BenchComparisonDelta>& deltas,
                      const std::string& model,
                      const std::string& previous_file,
                      const std::string& previous_timestamp);

// Build comparison JSON (for --json --compare)
json build_comparison_json(const std::vector<BenchBackendResult>& results,
                           const std::string& model,
                           const std::string& timestamp,
                           const BenchConfig& config,
                           const json& previous_results,
                           const std::vector<BenchComparisonDelta>& deltas);

// ============================================================
// Utility
// ============================================================

// Get ISO 8601 timestamp string
std::string get_timestamp_iso();

} // namespace lemon_cli

#endif // LEMON_CLI_BENCH_H
