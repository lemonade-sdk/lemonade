#include "lemon_cli/bench_comparison.h"
#include "lemon_cli/bench_output.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace lemon_cli {

using json = nlohmann::json;

std::map<std::string, json> extract_models_from_results(const json& root) {
    std::map<std::string, json> by_model;

    if (root.contains("models") && root["models"].is_array()) {
        for (const auto& model_entry : root["models"]) {
            if (model_entry.contains("model") && model_entry["model"].is_string()) {
                by_model[model_entry["model"].get<std::string>()] = model_entry;
            }
        }
        return by_model;
    }

    if (root.contains("model") && root["model"].is_string()) {
        by_model[root["model"].get<std::string>()] = root;
    }

    return by_model;
}

std::string extract_results_timestamp(const json& root) {
    if (root.contains("timestamp") && root["timestamp"].is_string()) {
        return root["timestamp"].get<std::string>();
    }
    return "";
}

std::string join_list(const std::vector<std::string>& values) {
    if (values.empty()) return "-";
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << values[i];
    }
    return oss.str();
}

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

    std::unordered_set<PrevKey, PrevKeyHash> matched_prev;
    std::vector<BenchComparisonDelta> deltas;

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
                delta.status = "failed";
                delta.ttft_pct_change = 0.0;
                delta.tps_pct_change = 0.0;
                delta.vram_gb_change = std::nullopt;
            } else if (it != prev_map.end()) {
                matched_prev.insert(key);

                if (it->second.value("all_runs_failed", false)) {
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
                    delta.vram_gb_change = (prev_vram >= 0 && curr_vram >= 0)
                        ? std::optional<double>(curr_vram - prev_vram)
                        : std::nullopt;
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
                           const std::string& timestamp,
                           const BenchConfig& config,
                           const json& previous_results,
                           const std::vector<BenchComparisonDelta>& deltas) {
    json output = to_json(results, model, timestamp, config);

    output["compare_file"] = config.compare_file;
    if (previous_results.contains("timestamp")) {
        output["previous_timestamp"] = previous_results["timestamp"];
    }
    output["previous_results"] =
        (previous_results.contains("results") && previous_results["results"].is_array())
            ? previous_results["results"]
            : json::array();

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

} // namespace lemon_cli
