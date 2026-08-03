#include "lemon_cli/bench_output.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace lemon_cli {

using json = nlohmann::json;

double percentile(std::vector<double> values, double p) {
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

std::string fmt_double(double val, int precision) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << val;
    return oss.str();
}

std::string fmt_vram(double val) {
    if (val < 0) return "-";
    return fmt_double(val, 1);
}

std::string fmt_pct_change(double pct) {
    std::ostringstream oss;
    oss << (pct >= 0 ? "+" : "") << std::fixed << std::setprecision(1) << pct << "%";
    return oss.str();
}

std::string fmt_vram_change(std::optional<double> val) {
    if (!val.has_value()) return "-";
    if (*val >= 0) return "+" + fmt_double(*val) + " GB";
    return fmt_double(*val) + " GB";
}

static size_t calculate_number_width(double value, int precision) {
    if (value < 0) {
        value = -value;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str().length();
}

static size_t calculate_vram_width(double value) {
    if (value < 0) return 1;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << value;
    return oss.str().length();
}

FieldWidths calculate_field_widths(const std::vector<BenchBackendResult>& results) {
    FieldWidths widths;

    for (const auto& backend_result : results) {
        for (const auto& scenario : backend_result.scenarios) {
            size_t scenario_len = scenario.scenario_name.length();
            if (scenario_len > 0) {
                if (scenario.failed_runs > 0) {
                    scenario_len += 2 + std::to_string(scenario.failed_runs).length() + 1;
                }
                if (scenario_len > widths.scenario_name) {
                    widths.scenario_name = std::min<size_t>(scenario_len, 36)+3;
                }
            }
        }
    }

    for (const auto& backend_result : results) {
        for (const auto& scenario : backend_result.scenarios) {
            if (scenario.runs.empty()) {
                widths.ttft = std::max(widths.ttft, size_t{8});
                widths.tps  = std::max(widths.tps,  size_t{8});
            } else {
                double max_ttft = scenario.ttft_max_ms();
                widths.ttft = std::max(widths.ttft, calculate_number_width(max_ttft, 1));

                double max_tps = scenario.tps_max();
                widths.tps = std::max(widths.tps, calculate_number_width(max_tps, 1));
            }

            double vram_peak = scenario.vram_peak_gb();
            if (vram_peak >= 0) {
                widths.vram = std::max(widths.vram, calculate_vram_width(vram_peak));
            }
        }
    }

    return widths;
}

static void print_scenario_row(const BenchScenarioResult& scenario, bool use_percentiles, const FieldWidths& widths) {
    double ttft_1 = use_percentiles ? scenario.ttft_p50_ms() : scenario.ttft_min_ms();
    double ttft_2 = use_percentiles ? scenario.ttft_p95_ms() : scenario.ttft_max_ms();
    double tps_1 = use_percentiles ? scenario.tps_p50() : scenario.tps_min();
    double tps_2 = use_percentiles ? scenario.tps_p95() : scenario.tps_max();
    std::string name = scenario.scenario_name;
    if (scenario.failed_runs > 0) {
        name += " *" + std::to_string(scenario.failed_runs) + "f ";
    }
    if (scenario.runs.empty()) {
        std::cout << std::left << std::setw(widths.scenario_name) << name
                  << std::setw(widths.ttft) << "-"
                  << std::setw(widths.ttft) << "-"
                  << std::setw(widths.ttft) << "-"
                  << std::setw(widths.tps) << "-"
                  << std::setw(widths.tps) << "-"
                  << std::setw(widths.tps) << "-"
                  << std::setw(widths.vram) << "-"
                  << std::endl;
    } else {
        std::cout << std::left << std::setw(widths.scenario_name) << name
                  << std::setw(widths.ttft) << fmt_double(scenario.ttft_mean_ms())
                  << " " << std::setw(widths.ttft) << fmt_double(ttft_1)
                  << " " << std::setw(widths.ttft) << fmt_double(ttft_2)
                  << " " << std::setw(widths.tps) << fmt_double(scenario.tps_mean())
                  << " " << std::setw(widths.tps) << fmt_double(tps_1)
                  << " " << std::setw(widths.tps) << fmt_double(tps_2)
                  << " " << std::setw(widths.vram) << fmt_vram(scenario.vram_peak_gb())
                  << std::endl;
    }
}

void print_table(const std::vector<BenchBackendResult>& results, const std::string& model,
                 bool use_percentiles) {
    FieldWidths widths = calculate_field_widths(results);
    std::cout << std::endl;
    std::cout << "Benchmark: " << model << std::endl;
    std::cout << std::string(100, '=') << std::endl;

    for (const auto& backend_result : results) {
        std::cout << std::endl;
        std::cout << "Backend: " << backend_result.label() << std::endl;
        std::cout << std::string(100, '-') << std::endl;

        std::cout << std::left << std::setw(widths.scenario_name) << "Scenario"
                  << std::setw(widths.ttft) << "TTFT"
                  << " " << std::setw(widths.ttft) << (use_percentiles ? "p50" : "min")
                  << " " << std::setw(widths.ttft) << (use_percentiles ? "p95" : "max")
                  << " " << std::setw(widths.tps) << "TPS"
                  << " " << std::setw(widths.tps) << (use_percentiles ? "p50" : "min")
                  << " " << std::setw(widths.tps) << (use_percentiles ? "p95" : "max")
                  << " " << std::setw(widths.vram) << "VRAM (GB)" << std::endl;
        std::cout << std::string(100, '-') << std::endl;

        for (const auto& scenario : backend_result.scenarios) {
            print_scenario_row(scenario, use_percentiles, widths);
        }
    }

    std::cout << std::endl;
    std::cout << "(*Nf = N failed runs excluded from stats)" << std::endl;
    std::cout << std::endl;
}

json to_json(const std::vector<BenchBackendResult>& results,
             const std::string& model,
             const std::string& timestamp,
             const BenchConfig& config) {
    json output;
    output["model"] = model;
    output["timestamp"] = timestamp;

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
                s_json["all_runs_failed"] = true;
            } else {
                json ttft_stats;
                ttft_stats["mean"] = scenario.ttft_mean_ms();
                ttft_stats["min"] = scenario.ttft_min_ms();
                ttft_stats["max"] = scenario.ttft_max_ms();
                ttft_stats["p50"] = scenario.ttft_p50_ms();
                ttft_stats["p95"] = scenario.ttft_p95_ms();
                s_json["ttft_ms"] = ttft_stats;

                std::vector<double> duration_vals;
                duration_vals.reserve(scenario.runs.size());
                double duration_sum = 0.0;
                double duration_min = scenario.runs[0].total_time_ms;
                double duration_max = scenario.runs[0].total_time_ms;
                for (const auto& run : scenario.runs) {
                    duration_vals.push_back(run.total_time_ms);
                    duration_sum += run.total_time_ms;
                    if (run.total_time_ms < duration_min) duration_min = run.total_time_ms;
                    if (run.total_time_ms > duration_max) duration_max = run.total_time_ms;
                }
                json duration_stats;
                duration_stats["mean"] = duration_sum / static_cast<double>(scenario.runs.size());
                duration_stats["min"] = duration_min;
                duration_stats["max"] = duration_max;
                duration_stats["p50"] = percentile(duration_vals, 50.0);
                duration_stats["p95"] = percentile(duration_vals, 95.0);
                s_json["duration_ms"] = duration_stats;

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

} // namespace lemon_cli
