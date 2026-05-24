#include "lemon_cli/bench.h"
#include "lemon_cli/lemonade_client.h"
#include <CLI/CLI.hpp>
#include <lemon/utils/path_utils.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace lemon_cli {

using json = nlohmann::json;
using namespace std::chrono;

// ============================================================
// Utility
// ============================================================

std::string get_timestamp_iso() {
    auto now = system_clock::now();
    auto time_t_now = system_clock::to_time_t(now);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return std::string(buf);
}

// ============================================================
// Statistics helpers
// ============================================================

static double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    if (values.size() == 1) return values[0];
    double index = (p / 100.0) * (static_cast<double>(values.size()) - 1.0);
    size_t lower = static_cast<size_t>(std::floor(index));
    size_t upper = static_cast<size_t>(std::ceil(index));
    if (lower == upper) return values[lower];
    double frac = index - static_cast<double>(lower);
    return values[lower] * (1.0 - frac) + values[upper] * frac;
}

double BenchScenarioResult::ttft_mean_ms() const {
    if (runs.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& r : runs) sum += r.ttft_ms;
    return sum / runs.size();
}

double BenchScenarioResult::ttft_min_ms() const {
    if (runs.empty()) return 0.0;
    double min_val = runs[0].ttft_ms;
    for (const auto& r : runs) if (r.ttft_ms < min_val) min_val = r.ttft_ms;
    return min_val;
}

double BenchScenarioResult::ttft_max_ms() const {
    if (runs.empty()) return 0.0;
    double max_val = runs[0].ttft_ms;
    for (const auto& r : runs) if (r.ttft_ms > max_val) max_val = r.ttft_ms;
    return max_val;
}

double BenchScenarioResult::ttft_p50_ms() const {
    std::vector<double> vals;
    for (const auto& r : runs) vals.push_back(r.ttft_ms);
    return percentile(vals, 50.0);
}

double BenchScenarioResult::ttft_p95_ms() const {
    std::vector<double> vals;
    for (const auto& r : runs) vals.push_back(r.ttft_ms);
    return percentile(vals, 95.0);
}

double BenchScenarioResult::tps_mean() const {
    if (runs.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& r : runs) sum += r.tps;
    return sum / runs.size();
}

double BenchScenarioResult::tps_min() const {
    if (runs.empty()) return 0.0;
    double min_val = runs[0].tps;
    for (const auto& r : runs) if (r.tps < min_val) min_val = r.tps;
    return min_val;
}

double BenchScenarioResult::tps_max() const {
    if (runs.empty()) return 0.0;
    double max_val = runs[0].tps;
    for (const auto& r : runs) if (r.tps > max_val) max_val = r.tps;
    return max_val;
}

double BenchScenarioResult::tps_p50() const {
    std::vector<double> vals;
    for (const auto& r : runs) vals.push_back(r.tps);
    return percentile(vals, 50.0);
}

double BenchScenarioResult::tps_p95() const {
    std::vector<double> vals;
    for (const auto& r : runs) vals.push_back(r.tps);
    return percentile(vals, 95.0);
}

double BenchScenarioResult::vram_peak_gb() const {
    double peak = -1.0;
    for (const auto& r : runs) {
        if (r.vram_gb > peak) peak = r.vram_gb;
    }
    return peak;
}

double BenchScenarioResult::memory_peak_gb() const {
    double peak = -1.0;
    for (const auto& r : runs) {
        if (r.memory_gb > peak) peak = r.memory_gb;
    }
    return peak;
}

int BenchScenarioResult::input_tokens() const {
    return runs.empty() ? 0 : runs[0].input_tokens;
}

int BenchScenarioResult::output_tokens() const {
    return runs.empty() ? 0 : runs[0].output_tokens;
}

std::string BenchBackendResult::label() const {
    std::string s = recipe + "/" + backend;
    if (ctx_size > 0) s += " (ctx=" + std::to_string(ctx_size) + ")";
    if (!backend_args.empty()) s += " args=[" + backend_args + "]";
    return s;
}

// ============================================================
// Scenario Loading
// ============================================================

static std::string extract_user_content(const std::vector<json>& messages) {
    for (const auto& msg : messages) {
        if (msg.contains("role") && msg["role"] == "user" && msg.contains("content")) {
            if (msg["content"].is_string()) {
                return msg["content"].get<std::string>();
            }
        }
    }
    return "Answer the question.";
}

std::string expand_context(const json& context_block, const std::vector<json>& messages) {
    std::string filler = context_block.value("filler", "");
    int target_tokens = context_block.value("target_tokens", 2000);
    std::string position = context_block.value("position", "end");

    if (filler.empty()) return "";

    // Estimate ~4 chars per token for English
    size_t target_chars = static_cast<size_t>(target_tokens) * 4;
    size_t filler_len = filler.size();
    if (filler_len == 0) return "";

    // Repeat filler to reach target
    std::string expanded;
    expanded.reserve(target_chars);
    size_t reps = (target_chars + filler_len - 1) / filler_len;
    for (size_t i = 0; i < reps; ++i) {
        expanded += filler;
    }

    // Extract the question content
    std::string question = extract_user_content(messages);

    if (position == "start") {
        return question + "\n\n" + expanded;
    } else if (position == "middle") {
        size_t mid = expanded.size() / 2;
        return expanded.substr(0, mid) + "\n\n" + question + "\n\n" + expanded.substr(mid);
    } else {
        // "end" (default)
        return expanded + "\n\n" + question;
    }
}

static std::vector<BenchScenario> parse_scenario_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Warning: Could not open scenario file: " << path << std::endl;
        return {};
    }

    json data;
    try {
        data = json::parse(file);
    } catch (const json::exception& e) {
        std::cerr << "Warning: Failed to parse scenario file " << path << ": " << e.what() << std::endl;
        return {};
    }

    std::vector<BenchScenario> scenarios;

    if (!data.contains("scenarios") || !data["scenarios"].is_array()) {
        std::cerr << "Warning: No 'scenarios' array in " << path << std::endl;
        return {};
    }

    for (const auto& item : data["scenarios"]) {
        BenchScenario scenario;

        if (!item.contains("name") || !item["name"].is_string()) continue;
        scenario.name = item["name"].get<std::string>();

        scenario.category = item.value("category", "general");

        if (!item.contains("messages") || !item["messages"].is_array()) continue;
        scenario.messages = item["messages"].get<std::vector<json>>();

        scenario.max_tokens = item.value("max_tokens", 128);
        scenario.warmup_runs = item.value("warmup_runs", 0);
        scenario.measurement_runs = item.value("measurement_runs", 3);

        // Handle context expansion for long-context scenarios
        if (item.contains("context") && item["context"].is_object()) {
            std::string expanded = expand_context(item["context"], scenario.messages);
            // Replace the user message content with the expanded text
            for (auto& msg : scenario.messages) {
                if (msg.contains("role") && msg["role"] == "user") {
                    if (msg.contains("content") && msg["content"].is_string()) {
                        msg["content"] = expanded;
                    }
                    break;
                }
            }
        }

        scenarios.push_back(scenario);
    }

    return scenarios;
}

std::vector<BenchScenario> load_scenarios_from_file(const std::string& path) {
    return parse_scenario_file(path);
}

std::vector<BenchScenario> load_scenarios_from_dir(const std::string& path) {
    std::vector<BenchScenario> all;
    std::unordered_set<std::string> seen_names;

    // Use std::filesystem to iterate directory
    std::vector<std::string> entries;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_regular_file()) {
                entries.push_back(entry.path().filename().string());
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not read scenario directory " << path << ": " << e.what() << std::endl;
        return {};
    }

    // Sort entries alphabetically for deterministic ordering
    std::sort(entries.begin(), entries.end());

    for (const auto& entry : entries) {
        if (entry.size() < 5 || entry.substr(entry.size() - 5) != ".json") continue;
        std::string full_path = path + "/" + entry;
        auto scenarios = parse_scenario_file(full_path);
        for (auto& s : scenarios) {
            if (seen_names.count(s.name)) {
                std::cerr << "Warning: Duplicate scenario name '" << s.name
                          << "' (from " << entry << "), skipping." << std::endl;
                continue;
            }
            seen_names.insert(s.name);
            all.push_back(std::move(s));
        }
    }

    return all;
}

std::string resolve_default_scenario_file() {
    // Try resources path relative to executable
    std::string path = lemon::utils::get_resource_path("resources/bench_scenarios.json");
    std::ifstream test(path);
    if (test.good()) {
        return path;
    }

    // Fallback: relative to current directory
    return "bench_scenarios.json";
}

// Check if a scenario matches a filter token.
// A token matches if it equals the scenario name, equals the scenario category,
// or equals "all" (matches every scenario).
static bool scenario_matches_filter(const BenchScenario& s, const std::string& token) {
    if (token == "all") return true;
    if (s.name == token) return true;
    if (s.category == token) return true;
    return false;
}

std::vector<BenchScenario> filter_scenarios(const std::vector<BenchScenario>& all,
                                            const std::vector<std::string>& tokens) {
    if (tokens.empty()) return all;
    std::vector<BenchScenario> filtered;
    for (const auto& s : all) {
        for (const auto& token : tokens) {
            if (scenario_matches_filter(s, token)) {
                filtered.push_back(s);
                break;  // avoid duplicates if multiple tokens match
            }
        }
    }
    return filtered;
}

// Return scenarios excluding the given category (e.g. "long-context").
static std::vector<BenchScenario> exclude_category(const std::vector<BenchScenario>& all,
                                                    const std::string& category) {
    std::vector<BenchScenario> filtered;
    for (const auto& s : all) {
        if (s.category != category) {
            filtered.push_back(s);
        }
    }
    return filtered;
}

// ============================================================
// Backend Discovery
// ============================================================

std::vector<BackendDiscovery> discover_backends(lemonade::LemonadeClient& client,
                                                const std::string& model,
                                                const std::vector<std::string>& requested) {
    std::vector<BackendDiscovery> result;

    try {
        std::string response = client.make_request("/api/v1/system-info", "GET", "", "", 10000, 10000);
        auto sys_info = json::parse(response);

        if (!sys_info.contains("recipes") || !sys_info["recipes"].is_object()) {
            std::cerr << "Warning: No recipes found in system-info" << std::endl;
            return result;
        }

        // Also get model info to find its recipe
        json model_info = client.get_model_info(model);
        std::string model_recipe;
        if (model_info.contains("recipe") && model_info["recipe"].is_string()) {
            model_recipe = model_info["recipe"].get<std::string>();
        }

        for (const auto& [recipe_name, recipe_data] : sys_info["recipes"].items()) {
            // If model has a specific recipe, only consider that recipe
            if (!model_recipe.empty() && recipe_name != model_recipe) {
                continue;
            }

            if (!recipe_data.contains("backends") || !recipe_data["backends"].is_object()) {
                continue;
            }

            for (const auto& [backend_name, backend_data] : recipe_data["backends"].items()) {
                std::string state = backend_data.value("state", "unknown");
                if (state != "installed" && state != "update_required" && state != "update_available") continue;

                // If specific backends requested, filter
                if (!requested.empty()) {
                    bool found = false;
                    for (const auto& req : requested) {
                        if (req == backend_name) { found = true; break; }
                    }
                    if (!found) continue;
                }

                result.push_back({recipe_name, backend_name});
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to discover backends: " << e.what() << std::endl;
    }

    return result;
}

// ============================================================
// Model Load/Unload
// ============================================================

// Map recipe name to the recipe_options key for custom args
static const std::map<std::string, std::string> RECIPE_ARGS_KEY = {
    {"llamacpp", "llamacpp_args"},
    {"flm", "flm_args"},
    {"vllm", "vllm_args"},
    {"sd-cpp", "sdcpp_args"},
    {"whispercpp", "whispercpp_args"},
};

bool load_model_for_backend(lemonade::LemonadeClient& client,
                            const std::string& model,
                            const std::string& recipe,
                            const std::string& backend,
                            int ctx_size,
                            const std::string& backend_args) {
    try {
        json request_body;
        request_body["model_name"] = model;
        request_body["save_options"] = false;

        // For llamacpp recipe, pass backend override
        if (recipe == "llamacpp") {
            request_body["llamacpp_backend"] = backend;
        }

        if (ctx_size > 0) {
            request_body["ctx_size"] = ctx_size;
        }

        // Pass backend-specific custom args if provided
        if (!backend_args.empty()) {
            auto it = RECIPE_ARGS_KEY.find(recipe);
            if (it != RECIPE_ARGS_KEY.end()) {
                request_body[it->second] = backend_args;
                // Disable merge so explicit benchmark args fully override defaults
                request_body["merge_args"] = false;
            }
        }

        BenchBackendResult tmp;
        tmp.recipe = recipe;
        tmp.backend = backend;
        tmp.ctx_size = ctx_size;
        tmp.backend_args = backend_args;
        std::cout << "  Loading model with " << tmp.label() << "..." << std::flush;

        // Long timeout for model loading
        client.make_request("/api/v1/load", "POST", request_body.dump(), "application/json",
                            86400000, 86400000);

        std::cout << " done" << std::endl;
        return true;
    } catch (const lemonade::HttpError& e) {
        std::cerr << " failed: " << lemonade::extract_server_error_message(e) << std::endl;
        return false;
    } catch (const std::exception& e) {
        std::cerr << " failed: " << e.what() << std::endl;
        return false;
    }
}

bool unload_all_models(lemonade::LemonadeClient& client) {
    try {
        json request_body;
        client.make_request("/api/v1/unload", "POST", request_body.dump(), "application/json");
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to unload models: " << e.what() << std::endl;
        return false;
    }
}

// ============================================================
// Memory Tracking
// ============================================================

static void query_system_stats(lemonade::LemonadeClient& client, double& vram_gb, double& memory_gb) {
    vram_gb = -1.0;
    memory_gb = -1.0;
    try {
        std::string response = client.make_request("/api/v1/system-stats", "GET", "", "", 5000, 5000);
        auto stats = json::parse(response);
        if (stats.contains("vram_gb") && stats["vram_gb"].is_number()) {
            vram_gb = stats["vram_gb"].get<double>();
        }
        if (stats.contains("memory_gb") && stats["memory_gb"].is_number()) {
            memory_gb = stats["memory_gb"].get<double>();
        }
    } catch (...) {
        // Best-effort: memory tracking is optional
    }
}

// ============================================================
// Benchmark Execution
// ============================================================

// Extract common timing/token fields from a server "usage" JSON object.
static void extract_usage_into_result(const json& usage, BenchRunResult& result) {
    if (usage.contains("prompt_tokens")) {
        result.input_tokens = usage["prompt_tokens"].get<int>();
    }
    if (usage.contains("completion_tokens")) {
        result.output_tokens = usage["completion_tokens"].get<int>();
    }
    if (usage.contains("prefill_duration_ttft")) {
        result.ttft_ms = usage["prefill_duration_ttft"].get<double>() * 1000.0;
    }
    if (usage.contains("decoding_speed_tps")) {
        result.tps = usage["decoding_speed_tps"].get<double>();
    }
}

// Extract timing fields from llama.cpp-style "timings" JSON object.
static void extract_timings_into_result(const json& timings, BenchRunResult& result) {
    if (timings.contains("prompt_ms")) {
        result.ttft_ms = timings["prompt_ms"].get<double>();
    }
    if (timings.contains("predicted_per_second")) {
        result.tps = timings["predicted_per_second"].get<double>();
    }
    if (timings.contains("prompt_n")) {
        result.input_tokens = timings["prompt_n"].get<int>();
    }
    if (timings.contains("predicted_n")) {
        result.output_tokens = timings["predicted_n"].get<int>();
    }
}

BenchRunResult run_single_bench(lemonade::LemonadeClient& client,
                                const std::string& model,
                                const BenchScenario& scenario,
                                bool memory_tracking) {
    BenchRunResult result;
    result.success = false;  // assume failure until proven otherwise

    // Warmup memory stats query (discard — we take post-run snapshot)
    if (memory_tracking) {
        double _vram, _mem;
        query_system_stats(client, _vram, _mem);
    }

    // Build request body
    json request_body;
    request_body["model"] = model;
    request_body["messages"] = scenario.messages;
    request_body["max_completion_tokens"] = scenario.max_tokens;
    request_body["temperature"] = 0;

    std::string body = request_body.dump();
    auto start = steady_clock::now();

    try {
        std::string response = client.make_request("/api/v1/chat/completions", "POST", body, "application/json",
                                                   300000, 300000);
        auto resp_json = json::parse(response);

        if (resp_json.contains("timings") && resp_json["timings"].is_object()) {
            extract_timings_into_result(resp_json["timings"], result);
        }
        if (resp_json.contains("usage") && resp_json["usage"].is_object()) {
            extract_usage_into_result(resp_json["usage"], result);
        }
    } catch (const std::exception& e) {
        std::cerr << "    Benchmark run failed: " << e.what() << std::endl;
        return result;
    }

    auto end = steady_clock::now();
    result.total_time_ms = duration<double, std::milli>(end - start).count();

    // Last resort: derive TPS from total time
    if (result.tps <= 0 && result.output_tokens > 0 && result.total_time_ms > 0) {
        result.tps = (result.output_tokens * 1000.0) / result.total_time_ms;
    }

    // Query memory after
    if (memory_tracking) {
        double vram_after = -1.0, mem_after = -1.0;
        query_system_stats(client, vram_after, mem_after);
        result.vram_gb = vram_after;
        result.memory_gb = mem_after;
    }

    // Validate: if all key metrics are zero the run failed server-side
    // (e.g. context size mismatch, model error). VRAM is excluded — it's
    // always reported correctly even for failed runs.
    if (result.ttft_ms <= 0 && result.tps <= 0 &&
        result.input_tokens <= 0 && result.output_tokens <= 0) {
        std::cerr << "    Benchmark run failed (all metrics zero)" << std::endl;
        return result;  // success stays false
    }

    result.success = true;
    return result;
}

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
                                 const std::string& backend_args) {
    BenchScenarioResult result;
    result.scenario_name = scenario.name;
    result.category = scenario.category;

    // Load model (once if not reloading, or before each run if reloading)
    bool loaded = false;
    if (!reload) {
        unload_all_models(client);
        loaded = load_model_for_backend(client, model, recipe, backend, ctx_size, backend_args);
        if (!loaded) {
            std::cerr << "    Model failed to load, skipping scenario." << std::endl;
            return result;
        }
    }

    // Warmup runs
    for (int i = 0; i < warmup; ++i) {
        if (reload) {
            unload_all_models(client);
            if (!load_model_for_backend(client, model, recipe, backend, ctx_size, backend_args)) {
                result.failed_runs++;
                continue;
            }
        }
        std::cout << "    Warmup " << (i + 1) << "/" << warmup << "..." << std::flush;
        run_single_bench(client, model, scenario, false);
        std::cout << " done" << std::endl;
    }

    // Measurement runs
    for (int i = 0; i < runs; ++i) {
        if (reload) {
            unload_all_models(client);
            if (!load_model_for_backend(client, model, recipe, backend, ctx_size, backend_args)) {
                std::cerr << "    Run " << (i + 1) << "/" << runs << "... FAILED to load model" << std::endl;
                result.failed_runs++;
                continue;
            }
        }

        std::cout << "    Run " << (i + 1) << "/" << runs << "..." << std::flush;
        auto run_result = run_single_bench(client, model, scenario, memory_tracking);
        if (!run_result.success) {
            result.failed_runs++;
            std::cout << " FAILED (excluded from stats)" << std::endl;
            continue;
        }
        std::cout << " TTFT=" << std::fixed << std::setprecision(1) << run_result.ttft_ms << "ms"
                  << " TPS=" << std::fixed << std::setprecision(1) << run_result.tps << std::endl;
        result.runs.push_back(run_result);
    }

    return result;
}

// ============================================================
// Output Formatting
// ============================================================

// Write JSON to file. Returns true on success, false on error (with stderr message).
// If error_fatal is true, the error message says "Error"; otherwise "Warning".
static bool write_json_file(const std::string& path, const std::string& json_str, bool error_fatal) {
    std::ofstream out(path);
    if (!out.is_open()) {
        std::cerr << (error_fatal ? "Error" : "Warning")
                  << ": Could not write to " << path << std::endl;
        return false;
    }
    out << json_str;
    out.close();
    return true;
}

static std::string fmt_double(double val, int precision = 1) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << val;
    return oss.str();
}

static std::string fmt_vram(double val) {
    if (val < 0) return "-";
    return fmt_double(val, 1);
}

static std::string fmt_pct_change(double pct) {
    std::ostringstream oss;
    oss << (pct >= 0 ? "+" : "") << std::fixed << std::setprecision(1) << pct << "%";
    return oss.str();
}

static std::string fmt_vram_change(std::optional<double> val) {
    if (!val.has_value()) return "-";
    if (*val >= 0) return "+" + fmt_double(*val) + " GB";
    return fmt_double(*val) + " GB";
}

static void print_scenario_row(const BenchScenarioResult& scenario, bool use_percentiles) {
    double ttft_1 = use_percentiles ? scenario.ttft_p50_ms() : scenario.ttft_min_ms();
    double ttft_2 = use_percentiles ? scenario.ttft_p95_ms() : scenario.ttft_max_ms();
    double tps_1 = use_percentiles ? scenario.tps_p50() : scenario.tps_min();
    double tps_2 = use_percentiles ? scenario.tps_p95() : scenario.tps_max();
    std::string name = scenario.scenario_name;
    if (scenario.failed_runs > 0) {
        name += " *" + std::to_string(scenario.failed_runs) + "f";
    }
    if (scenario.runs.empty()) {
        // All runs failed — show dashes
        std::cout << std::left << std::setw(20) << name
                  << std::setw(8) << "-"
                  << std::setw(8) << "-"
                  << std::setw(8) << "-"
                  << std::setw(8) << "-"
                  << std::setw(8) << "-"
                  << std::setw(8) << "-"
                  << std::setw(8) << "-"
                  << std::endl;
    } else {
        std::cout << std::left << std::setw(20) << name
                  << std::setw(8) << fmt_double(scenario.ttft_mean_ms())
                  << std::setw(8) << fmt_double(ttft_1)
                  << std::setw(8) << fmt_double(ttft_2)
                  << std::setw(8) << fmt_double(scenario.tps_mean())
                  << std::setw(8) << fmt_double(tps_1)
                  << std::setw(8) << fmt_double(tps_2)
                  << std::setw(8) << fmt_vram(scenario.vram_peak_gb())
                  << std::endl;
    }
}

void print_table(const std::vector<BenchBackendResult>& results, const std::string& model,
                 bool use_percentiles) {
    std::cout << std::endl;
    std::cout << "Benchmark: " << model << std::endl;
    std::cout << std::string(100, '=') << std::endl;

    for (const auto& backend_result : results) {
        std::cout << std::endl;
        std::cout << "Backend: " << backend_result.label() << std::endl;
        std::cout << std::string(100, '-') << std::endl;

        // Header — show min/max by default, switch to p50/p95 when runs >= 10
        std::cout << std::left << std::setw(20) << "Scenario"
                  << std::setw(8) << "TTFT"
                  << std::setw(8) << (use_percentiles ? "p50" : "min")
                  << std::setw(8) << (use_percentiles ? "p95" : "max")
                  << std::setw(8) << "TPS"
                  << std::setw(8) << (use_percentiles ? "p50" : "min")
                  << std::setw(8) << (use_percentiles ? "p95" : "max")
                  << std::setw(8) << "VRAM (GB)" << std::endl;
        std::cout << std::string(100, '-') << std::endl;

        for (const auto& scenario : backend_result.scenarios) {
            print_scenario_row(scenario, use_percentiles);
        }
    }

    std::cout << std::endl;
    std::cout << "(*Nf = N failed runs excluded from stats)" << std::endl;
    std::cout << std::endl;
}

json to_json(const std::vector<BenchBackendResult>& results,
             const std::string& model,
             const BenchConfig& config) {
    json output;
    output["model"] = model;
    output["timestamp"] = get_timestamp_iso();

    json config_json;
    config_json["warmup_runs"] = config.warmup_runs;
    config_json["measurement_runs"] = config.measurement_runs;
    config_json["memory_tracking"] = config.memory_tracking;
    output["config"] = config_json;

    json results_json = json::array();
    for (const auto& backend_result : results) {
        json backend_json;
        backend_json["recipe"] = backend_result.recipe;
        backend_json["backend"] = backend_result.backend;
        backend_json["ctx_size"] = backend_result.ctx_size;
        backend_json["backend_args"] = backend_result.backend_args;

        json scenarios_json = json::array();
        for (const auto& scenario : backend_result.scenarios) {
            json s_json;
            s_json["name"] = scenario.scenario_name;
            s_json["category"] = scenario.category;
            s_json["input_tokens"] = scenario.input_tokens();
            s_json["output_tokens"] = scenario.output_tokens();

            if (scenario.runs.empty()) {
                // All runs failed — omit metric blocks so the result is clearly
                // distinguishable from a valid 0ms/0tps measurement.
                s_json["all_runs_failed"] = true;
            } else {
                json ttft_stats;
                ttft_stats["mean"] = scenario.ttft_mean_ms();
                ttft_stats["min"] = scenario.ttft_min_ms();
                ttft_stats["max"] = scenario.ttft_max_ms();
                ttft_stats["p50"] = scenario.ttft_p50_ms();
                ttft_stats["p95"] = scenario.ttft_p95_ms();
                s_json["ttft_ms"] = ttft_stats;

                json tps_stats;
                tps_stats["mean"] = scenario.tps_mean();
                tps_stats["min"] = scenario.tps_min();
                tps_stats["max"] = scenario.tps_max();
                tps_stats["p50"] = scenario.tps_p50();
                tps_stats["p95"] = scenario.tps_p95();
                s_json["tps"] = tps_stats;

                double vram_peak = scenario.vram_peak_gb();
                if (vram_peak >= 0) s_json["vram_peak_gb"] = vram_peak;
                double mem_peak = scenario.memory_peak_gb();
                if (mem_peak >= 0) s_json["memory_peak_gb"] = mem_peak;
            }
            s_json["failed_runs"] = scenario.failed_runs;

            scenarios_json.push_back(s_json);
        }
        backend_json["scenarios"] = scenarios_json;
        results_json.push_back(backend_json);
    }
    output["results"] = results_json;

    return output;
}

// ============================================================
// Comparison
// ============================================================

json load_previous_results(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open comparison file: " << file_path << std::endl;
        return json::object();
    }

    try {
        return json::parse(file);
    } catch (const json::exception& e) {
        std::cerr << "Error: Failed to parse comparison file: " << e.what() << std::endl;
        return json::object();
    }
}

std::vector<BenchComparisonDelta> compute_deltas(const std::vector<BenchBackendResult>& current,
                                                  const json& previous_results) {
    std::vector<BenchComparisonDelta> deltas;

    // Build lookup map from previous results
    struct PrevKey {
        std::string backend;
        int ctx_size = 0;
        std::string backend_args;
        std::string scenario;
        bool operator==(const PrevKey& o) const {
            return backend == o.backend && ctx_size == o.ctx_size && backend_args == o.backend_args && scenario == o.scenario;
        }
    };
    struct PrevKeyHash {
        size_t operator()(const PrevKey& k) const {
            return std::hash<std::string>()(k.backend)
                 ^ (std::hash<int>()(k.ctx_size) << 1)
                 ^ (std::hash<std::string>()(k.backend_args) << 2)
                 ^ (std::hash<std::string>()(k.scenario) << 3);
        }
    };
    std::unordered_map<PrevKey, json, PrevKeyHash> prev_map;
    std::unordered_set<PrevKey, PrevKeyHash> prev_keys;

    if (previous_results.contains("results") && previous_results["results"].is_array()) {
        for (const auto& prev_backend : previous_results["results"]) {
            std::string recipe = prev_backend.value("recipe", "");
            std::string backend = prev_backend.value("backend", "");
            std::string backend_label = recipe + "/" + backend;
            int prev_ctx_size = prev_backend.value("ctx_size", 0);
            std::string prev_backend_args = prev_backend.value("backend_args", "");

            if (prev_backend.contains("scenarios") && prev_backend["scenarios"].is_array()) {
                for (const auto& prev_scenario : prev_backend["scenarios"]) {
                    std::string name = prev_scenario.value("name", "");
                    PrevKey key{backend_label, prev_ctx_size, prev_backend_args, name};
                    prev_map[key] = prev_scenario;
                    prev_keys.insert(key);
                }
            }
        }
    }

    // Track which previous keys we've matched
    std::unordered_set<PrevKey, PrevKeyHash> matched_prev;

    // Iterate current results
    for (const auto& backend_result : current) {
        std::string backend_label = backend_result.recipe + "/" + backend_result.backend;

        for (const auto& scenario : backend_result.scenarios) {
            PrevKey key{backend_label, backend_result.ctx_size, backend_result.backend_args, scenario.scenario_name};
            auto it = prev_map.find(key);

            BenchComparisonDelta delta;
            delta.backend = backend_label;
            delta.ctx_size = backend_result.ctx_size;
            delta.backend_args = backend_result.backend_args;
            delta.scenario = scenario.scenario_name;

            if (scenario.runs.empty()) {
                // All runs failed — no meaningful delta to compute
                delta.status = "failed";
                delta.ttft_pct_change = 0.0;
                delta.tps_pct_change = 0.0;
                delta.vram_gb_change = std::nullopt;
            } else if (it != prev_map.end()) {
                matched_prev.insert(key);

                if (it->second.value("all_runs_failed", false)) {
                    // Previous run failed — current succeeded but no baseline to compare
                    delta.status = "prev_failed";
                    delta.ttft_pct_change = 0.0;
                    delta.tps_pct_change = 0.0;
                    delta.vram_gb_change = std::nullopt;
                } else {
                    delta.status = "matched";

                    double curr_ttft = scenario.ttft_mean_ms();
                    double curr_tps = scenario.tps_mean();
                    double curr_vram = scenario.vram_peak_gb();

                    double prev_ttft = it->second.value("ttft_ms", json::object()).value("mean", 0.0);
                    double prev_tps = it->second.value("tps", json::object()).value("mean", 0.0);
                    double prev_vram = it->second.value("vram_peak_gb", -1.0);

                    delta.ttft_pct_change = (prev_ttft > 0) ? ((curr_ttft - prev_ttft) / prev_ttft * 100.0) : 0.0;
                    delta.tps_pct_change = (prev_tps > 0) ? ((curr_tps - prev_tps) / prev_tps * 100.0) : 0.0;
                    delta.vram_gb_change = (prev_vram >= 0 && curr_vram >= 0) ? std::optional<double>(curr_vram - prev_vram) : std::nullopt;
                }
            } else {
                delta.status = "new";
                delta.ttft_pct_change = 0.0;
                delta.tps_pct_change = 0.0;
                delta.vram_gb_change = std::nullopt;
            }

            deltas.push_back(delta);
        }
    }

    // Mark unmatched previous results as "removed"
    for (const auto& prev_key : prev_keys) {
        if (matched_prev.find(prev_key) == matched_prev.end()) {
            BenchComparisonDelta delta;
            delta.backend = prev_key.backend;
            delta.ctx_size = prev_key.ctx_size;
            delta.backend_args = prev_key.backend_args;
            delta.scenario = prev_key.scenario;
            delta.status = "removed";
            delta.ttft_pct_change = 0.0;
            delta.tps_pct_change = 0.0;
            delta.vram_gb_change = std::nullopt;
            deltas.push_back(delta);
        }
    }

    return deltas;
}

void print_comparison(const std::vector<BenchComparisonDelta>& deltas,
                      const std::string& model,
                      const std::string& previous_file,
                      const std::string& previous_timestamp) {
    std::cout << std::endl;
    std::cout << "Comparison: " << model << std::endl;
    std::cout << "Previous: " << previous_file
              << (previous_timestamp.empty() ? "" : " (" + previous_timestamp + ")") << std::endl;
    std::cout << "Current:  " << get_timestamp_iso() << std::endl;
    std::cout << std::string(100, '=') << std::endl;

    // Group by backend + ctx_size + backend_args
    std::map<std::string, std::vector<const BenchComparisonDelta*>> by_backend;
    for (const auto& d : deltas) {
        std::string label = d.backend;
        if (d.ctx_size > 0) label += " (ctx=" + std::to_string(d.ctx_size) + ")";
        if (!d.backend_args.empty()) label += " args=[" + d.backend_args + "]";
        by_backend[label].push_back(&d);
    }

    for (const auto& [label, backend_deltas] : by_backend) {
        std::cout << std::endl;
        std::cout << "Backend: " << label << std::endl;
        std::cout << std::string(80, '-') << std::endl;
        std::cout << std::left << std::setw(22) << "Scenario"
                  << std::setw(14) << "TTFT change"
                  << std::setw(14) << "TPS change"
                  << std::setw(16) << "VRAM change"
                  << "Status" << std::endl;
        std::cout << std::string(80, '-') << std::endl;

        for (const auto* d : backend_deltas) {
            std::string ttft_str = "-", tps_str = "-", vram_str = "-";
            if (d->status == "matched") {
                ttft_str = fmt_pct_change(d->ttft_pct_change);
                tps_str = fmt_pct_change(d->tps_pct_change);
                vram_str = fmt_vram_change(d->vram_gb_change);
            }
            // "failed" and "new" statuses keep the default "-" values

            std::cout << std::left << std::setw(22) << d->scenario
                      << std::setw(14) << ttft_str
                      << std::setw(14) << tps_str
                      << std::setw(16) << vram_str
                      << "(" << d->status << ")" << std::endl;
        }
    }

    std::cout << std::endl;
    std::cout << "Legend: TTFT change > 0 means slower (worse). TPS change > 0 means faster (better)." << std::endl;
    std::cout << "Status: matched = compared against previous, new = no previous data, removed = not in current run, failed = all runs errored, prev_failed = previous run errored" << std::endl;
    std::cout << std::endl;
}

json build_comparison_json(const std::vector<BenchBackendResult>& results,
                           const std::string& model,
                           const BenchConfig& config,
                           const json& previous_results,
                           const std::vector<BenchComparisonDelta>& deltas) {
    json output = to_json(results, model, config);

    output["compare_file"] = config.compare_file;
    if (previous_results.contains("timestamp")) {
        output["previous_timestamp"] = previous_results["timestamp"];
    }
    output["previous_results"] = previous_results["results"];

    json comparison_json = json::array();
    for (const auto& d : deltas) {
        json d_json;
        d_json["backend"] = d.backend;
        d_json["ctx_size"] = d.ctx_size;
        d_json["backend_args"] = d.backend_args;
        d_json["scenario"] = d.scenario;
        d_json["ttft_pct_change"] = d.ttft_pct_change;
        d_json["tps_pct_change"] = d.tps_pct_change;
        if (d.vram_gb_change.has_value()) d_json["vram_gb_change"] = *d.vram_gb_change;
        else d_json["vram_gb_change"] = nullptr;
        d_json["status"] = d.status;
        comparison_json.push_back(d_json);
    }
    output["comparison"] = comparison_json;

    return output;
}

// ============================================================
// Main Bench Handler
// ============================================================

int handle_bench_command(lemonade::LemonadeClient& client, const BenchConfig& config) {
    // 1. Validate model exists
    json model_info = client.get_model_info(config.model);
    if (model_info.empty()) {
        std::cerr << "Error: Model '" << config.model << "' not found." << std::endl;
        if (config.auto_pull) {
            std::cout << "Auto-pulling model..." << std::endl;
            json pull_req;
            pull_req["model_name"] = config.model;
            if (client.pull_model(pull_req) != 0) {
                std::cerr << "Error: Failed to pull model." << std::endl;
                return 1;
            }
        } else {
            std::cerr << "Use --auto-pull to download it automatically." << std::endl;
            return 1;
        }
    } else {
        // Check if downloaded
        if (model_info.contains("downloaded") && !model_info["downloaded"].get<bool>()) {
            std::cerr << "Error: Model '" << config.model << "' is not downloaded." << std::endl;
            if (config.auto_pull) {
                std::cout << "Auto-pulling model..." << std::endl;
                json pull_req;
                pull_req["model_name"] = config.model;
                if (client.pull_model(pull_req) != 0) {
                    std::cerr << "Error: Failed to pull model." << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Run 'lemonade pull " << config.model << "' first, or use --auto-pull." << std::endl;
                return 1;
            }
        }
    }

    // 2. Load scenarios
    std::vector<BenchScenario> scenarios;
    if (!config.scenario_file.empty()) {
        scenarios = load_scenarios_from_file(config.scenario_file);
    } else if (!config.scenario_dir.empty()) {
        scenarios = load_scenarios_from_dir(config.scenario_dir);
    } else {
        std::string default_path = resolve_default_scenario_file();
        scenarios = load_scenarios_from_file(default_path);
    }

    if (scenarios.empty()) {
        std::cerr << "Error: No scenarios loaded." << std::endl;
        return 1;
    }

    // Filter scenarios
    // When no --scenarios filter is given, exclude long-context by default
    // (they run very long and fail on many systems).
    // When --scenarios is provided, match by name, category, or "all".
    if (config.scenario_names.empty()) {
        scenarios = exclude_category(scenarios, "long-context");
    } else {
        scenarios = filter_scenarios(scenarios, config.scenario_names);
    }
    if (scenarios.empty()) {
        std::cerr << "Error: No scenarios matched the filter." << std::endl;
        return 1;
    }

    // 3. Discover backends
    auto backends = discover_backends(client, config.model, config.backends);
    if (backends.empty()) {
        std::cerr << "Error: No suitable backends found for model '" << config.model << "'." << std::endl;
        return 1;
    }

    // 4. Determine context sizes
    std::vector<int> ctx_sizes = config.ctx_sizes;
    if (ctx_sizes.empty()) {
        // Use model's default context size from info, or a reasonable default
        int default_ctx = 4096;
        if (model_info.contains("recipe_options") && model_info["recipe_options"].is_object()) {
            auto& opts = model_info["recipe_options"];
            if (opts.contains("ctx_size") && opts["ctx_size"].is_number()) {
                default_ctx = opts["ctx_size"].get<int>();
            }
        }
        ctx_sizes = {default_ctx};
    }

    // 5. Run benchmarks
    std::vector<BenchBackendResult> all_results;

    for (const auto& [recipe, backend] : backends) {
        // Look up backend-specific args for this recipe
        // If none specified, use a single empty-string entry so we still iterate once
        std::vector<std::string> recipe_args_list;
        auto args_it = config.backend_args.find(recipe);
        if (args_it != config.backend_args.end() && !args_it->second.empty()) {
            recipe_args_list = args_it->second;
        } else {
            recipe_args_list = {""};  // default: no extra args
        }

        // Iterate all combinations of ctx_size × backend_args
        for (int ctx_size : ctx_sizes) {
            for (const auto& recipe_args : recipe_args_list) {
                BenchBackendResult tmp;
                tmp.recipe = recipe;
                tmp.backend = backend;
                tmp.ctx_size = ctx_size;
                tmp.backend_args = recipe_args;
                std::cout << "\n=== " << tmp.label() << " ===" << std::endl;

                BenchBackendResult backend_result;
                backend_result.recipe = recipe;
                backend_result.backend = backend;
                backend_result.ctx_size = ctx_size;
                backend_result.backend_args = recipe_args;

                for (const auto& scenario : scenarios) {
                    std::cout << "  Scenario: " << scenario.name << " (" << scenario.category << ")" << std::endl;

                    int warmup = scenario.warmup_runs;
                    int runs = scenario.measurement_runs;

                    // Allow config overrides
                    if (config.warmup_runs > 0) warmup = config.warmup_runs;
                    if (config.measurement_runs > 0) runs = config.measurement_runs;

                    auto scenario_result = run_scenario(client, config.model, scenario, warmup, runs,
                                                        config.memory_tracking, config.reload, recipe, backend, ctx_size, recipe_args);
                    backend_result.scenarios.push_back(scenario_result);
                }

                // Only add results if at least one scenario ran
                if (!backend_result.scenarios.empty()) {
                    all_results.push_back(backend_result);
                }
            }
        }
    }

    if (all_results.empty()) {
        std::cerr << "Error: No benchmark results (all backends failed to load)." << std::endl;
        return 1;
    }

    // 6. Output results
    if (config.compare_file.empty()) {
        // Normal output
        if (config.json_output) {
            json output = to_json(all_results, config.model, config);
            std::string json_str = output.dump(2);
            if (!config.output_file.empty()) {
                if (!write_json_file(config.output_file, json_str, true)) return 1;
                std::cout << "Results written to " << config.output_file << std::endl;
            } else {
                std::cout << json_str << std::endl;
            }
        } else {
            print_table(all_results, config.model, config.measurement_runs >= 10);
            if (!config.output_file.empty()) {
                json output = to_json(all_results, config.model, config);
                if (write_json_file(config.output_file, output.dump(2), false)) {
                    std::cout << "JSON results also written to " << config.output_file << std::endl;
                }
            }
        }
    } else {
        // Comparison mode
        json previous_results = load_previous_results(config.compare_file);
        if (previous_results.empty()) {
            std::cerr << "Error: Could not load previous results from " << config.compare_file << std::endl;
            return 1;
        }

        auto deltas = compute_deltas(all_results, previous_results);

        if (config.json_output) {
            json output = build_comparison_json(all_results, config.model, config,
                                                 previous_results, deltas);
            std::string json_str = output.dump(2);
            if (!config.output_file.empty()) {
                if (!write_json_file(config.output_file, json_str, true)) return 1;
                std::cout << "Results written to " << config.output_file << std::endl;
            } else {
                std::cout << json_str << std::endl;
            }
        } else {
            // Print normal table first, then comparison
            print_table(all_results, config.model, config.measurement_runs >= 10);
            print_comparison(deltas, config.model, config.compare_file,
                           previous_results.value("timestamp", ""));
        }
    }

    // Unload models on exit
    unload_all_models(client);

    return 0;
}

// ============================================================
// CLI helpers
// ============================================================

CLI::App* register_bench_command(CLI::App& parent,
                                 std::string& model,
                                 std::string& output_file,
                                 BenchCliOptions& opts) {
    CLI::App* cmd = parent.add_subcommand("bench", "Benchmark a model's chat completion performance")->group("Model management");
    cmd->add_option("model", model, "Model name to benchmark")->required()->type_name("MODEL");
    cmd->add_option("--backend", opts.backends, "Backend to test (e.g., vulkan, metal, cpu). Repeat for multiple.")
        ->type_name("BACKEND")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--ctx-size", opts.ctx_sizes, "Context size to test. Repeat for multiple sizes.")
        ->type_name("SIZE")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--runs", opts.runs, "Number of measurement runs per scenario (default: 3)")->type_name("N");
    cmd->add_option("--warmup", opts.warmup, "Number of warmup runs per scenario (default: 0)")->type_name("N");
    cmd->add_option("--scenarios", opts.scenario_names,
        "Scenario name(s) or category to run (e.g. chat, coding, long-context). "
        "Use 'all' to include every scenario including long-context. "
        "Default: all scenarios except long-context.")
        ->type_name("NAME|CATEGORY")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--scenario-file", opts.scenario_file, "Load scenarios from a single JSON file")->type_name("FILE");
    cmd->add_option("--scenario-dir", opts.scenario_dir, "Load all .json scenario files from a directory")->type_name("DIR");
    cmd->add_flag("--json", opts.json_output, "Output results as JSON");
    cmd->add_option("--output", output_file, "Write results to file (in addition to stdout)")->type_name("FILE");
    cmd->add_option("--compare", opts.compare_file, "Compare results against a previously saved JSON file")->type_name("FILE");
    cmd->add_flag("--auto-pull", opts.auto_pull, "Automatically pull the model if not downloaded");
    cmd->add_flag("--no-memory", opts.no_memory, "Disable VRAM/RAM tracking");
    cmd->add_flag("--no-reload", opts.no_reload, "Skip model reload between scenarios (faster but prompt cache may skew results)");
    cmd->add_option("--llamacpp-args", opts.llamacpp_args, "Custom args for llama-server (e.g. \"-b 2048 -ub 1024\"). Repeat for multiple.")
        ->type_name("ARGS")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--flm-args", opts.flm_args, "Custom args for flm serve. Repeat for multiple.")
        ->type_name("ARGS")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--vllm-args", opts.vllm_args, "Custom args for vllm-server. Repeat for multiple.")
        ->type_name("ARGS")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--sdcpp-args", opts.sdcpp_args, "Custom args for sd-server. Repeat for multiple.")
        ->type_name("ARGS")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--whispercpp-args", opts.whispercpp_args, "Custom args for whisper-server. Repeat for multiple.")
        ->type_name("ARGS")
        ->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    return cmd;
}

BenchConfig build_bench_config(const std::string& model,
                               const std::string& output_file,
                               const BenchCliOptions& cli) {
    BenchConfig config;
    config.model = model;
    config.backends = cli.backends;
    config.ctx_sizes = cli.ctx_sizes;
    config.warmup_runs = cli.warmup;
    config.measurement_runs = cli.runs;
    config.json_output = cli.json_output;
    config.output_file = output_file;
    config.scenario_file = cli.scenario_file;
    config.scenario_dir = cli.scenario_dir;
    config.auto_pull = cli.auto_pull;
    config.memory_tracking = !cli.no_memory;
    config.reload = !cli.no_reload;
    config.compare_file = cli.compare_file;
    config.scenario_names = cli.scenario_names;
    // Populate backend-specific args map (only non-empty values)
    if (!cli.llamacpp_args.empty()) config.backend_args["llamacpp"] = cli.llamacpp_args;
    if (!cli.flm_args.empty()) config.backend_args["flm"] = cli.flm_args;
    if (!cli.vllm_args.empty()) config.backend_args["vllm"] = cli.vllm_args;
    if (!cli.sdcpp_args.empty()) config.backend_args["sd-cpp"] = cli.sdcpp_args;
    if (!cli.whispercpp_args.empty()) config.backend_args["whispercpp"] = cli.whispercpp_args;
    return config;
}

} // namespace lemon_cli
