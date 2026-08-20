#include "lemon_cli/bench.h"
#include "lemon_cli/bench_comparison.h"
#include "lemon_cli/bench_output.h"
#include "lemon_cli/bench_sysinfo.h"
#include "lemon_cli/lemonade_client.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/model_types.h"
#include <CLI/CLI.hpp>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/path_utils.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <unordered_set>

namespace lemon_cli {

using json = nlohmann::json;
using namespace std::chrono;

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

template<typename T>
static std::vector<T> collect_field(const BenchScenarioResult& self, T (BenchRunResult::*field)) {
    std::vector<T> vals;
    vals.reserve(self.runs.size());
    for (const auto& r : self.runs) vals.push_back(r.*field);
    return vals;
}

template<typename T>
static T reduce_mean(std::vector<T> vals) {
    if (vals.empty()) return T{};
    return std::accumulate(vals.begin(), vals.end(), T{}) / static_cast<double>(vals.size());
}

template<typename T>
static T reduce_min(std::vector<T> vals) {
    if (vals.empty()) return T{};
    return *std::min_element(vals.begin(), vals.end());
}

template<typename T>
static T reduce_max(std::vector<T> vals) {
    if (vals.empty()) return T{};
    return *std::max_element(vals.begin(), vals.end());
}

template<typename T>
static T reduce_peak(std::vector<T> vals, T initial = T{-1}) {
    return reduce_max(vals.empty() ? std::vector<T>{initial} : vals);
}

double BenchScenarioResult::ttft_mean_ms() const { return reduce_mean(collect_field(*this, &BenchRunResult::ttft_ms)); }
double BenchScenarioResult::ttft_min_ms() const { return reduce_min(collect_field(*this, &BenchRunResult::ttft_ms)); }
double BenchScenarioResult::ttft_max_ms() const { return reduce_max(collect_field(*this, &BenchRunResult::ttft_ms)); }
double BenchScenarioResult::ttft_p50_ms() const { return percentile(collect_field(*this, &BenchRunResult::ttft_ms), 50.0); }
double BenchScenarioResult::ttft_p95_ms() const { return percentile(collect_field(*this, &BenchRunResult::ttft_ms), 95.0); }
double BenchScenarioResult::tps_mean() const { return reduce_mean(collect_field(*this, &BenchRunResult::tps)); }
double BenchScenarioResult::tps_min() const { return reduce_min(collect_field(*this, &BenchRunResult::tps)); }
double BenchScenarioResult::tps_max() const { return reduce_max(collect_field(*this, &BenchRunResult::tps)); }
double BenchScenarioResult::tps_p50() const { return percentile(collect_field(*this, &BenchRunResult::tps), 50.0); }
double BenchScenarioResult::tps_p95() const { return percentile(collect_field(*this, &BenchRunResult::tps), 95.0); }
double BenchScenarioResult::vram_peak_gb() const { return reduce_peak(collect_field(*this, &BenchRunResult::vram_gb)); }
double BenchScenarioResult::memory_peak_gb() const { return reduce_peak(collect_field(*this, &BenchRunResult::memory_gb)); }
int BenchScenarioResult::input_tokens() const { return runs.empty() ? 0 : runs[0].input_tokens; }
int BenchScenarioResult::output_tokens() const { return runs.empty() ? 0 : runs[0].output_tokens; }

std::string BenchBackendResult::label() const {
    std::string s = recipe + "/" + backend;
    if (recipe != "sd-cpp")
        if (ctx_size > 0) s += " (ctx=" + std::to_string(ctx_size) + ")";
    if (!backend_args.empty()) s += " args=[" + backend_args + "]";
    return s;
}

static bool is_vision_scenario(const BenchScenario& scenario) {
    return scenario.category == "vision";
}

static std::filesystem::path resolve_image_path(const BenchScenario& scenario) {
    if (scenario.image_path.empty()) {
        throw std::runtime_error("Vision scenario '" + scenario.name + "' is missing image_path");
    }
    std::filesystem::path source_dir =
        std::filesystem::absolute(std::filesystem::path(scenario.source_file).parent_path());
    std::filesystem::path image_file(scenario.image_path);
    std::filesystem::path resolved = image_file.is_absolute() ? image_file : source_dir / image_file;

    std::filesystem::path canon_dir = std::filesystem::weakly_canonical(source_dir);
    std::filesystem::path canon_res = std::filesystem::weakly_canonical(resolved);

    std::filesystem::path canon_image_dir = canon_res.parent_path();
    if (canon_image_dir != canon_dir) {
        throw std::runtime_error("Image path must be in the same directory as the scenario file: " +
                                image_file.string());
    }
    return canon_res;
}

static void embed_image_into_message(json& message, const BenchScenario& scenario) {
    if (!message.contains("role") || message["role"] != "user") return;

    auto abs_path = resolve_image_path(scenario);
    if (!std::filesystem::exists(abs_path)) {
        throw std::runtime_error("Image file not found: " + abs_path.string());
    }

    std::string image_data;
    {
        std::ifstream file(abs_path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Could not open image file: " + abs_path.string());
        }
        image_data.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }
    std::string b64_data = lemon::utils::JsonUtils::base64_encode(image_data);

    auto detect_mime_type = [](const std::filesystem::path& p) -> std::string {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".png") return "image/png";
        if (ext == ".webp") return "image/webp";
        if (ext == ".gif") return "image/gif";
        throw std::runtime_error("Unsupported image format: " + ext);
    };

    std::string mime_type = detect_mime_type(abs_path);

    json content_array = json::array();
    if (message.contains("content")) {
        if (message["content"].is_string()) {
            content_array.push_back({{"type", "text"}, {"text", message["content"].get<std::string>()}});
        } else if (message["content"].is_array()) {
            for (const auto& part : message["content"]) {
                content_array.push_back(part);
            }
        }
    }

    content_array.push_back({{"type", "image_url"}, {"image_url", {{ "url", "data:" + mime_type + ";base64," + b64_data }}}});
    message["content"] = content_array;
}

static bool model_has_vision_label(const json& model_info) {
    if (!model_info.contains("labels") || !model_info["labels"].is_array()) return false;
    for (const auto& label : model_info["labels"]) {
        if (label.is_string() && label.get<std::string>() == "vision") return true;
    }
    return false;
}

static std::string extract_user_content(const std::vector<json>& messages) {
    for (const auto& msg : messages) {
        if (msg.contains("role") && msg["role"] == "user" && msg.contains("content")) {
            if (msg["content"].is_string()) return msg["content"].get<std::string>();
        }
    }
    return "Answer the question.";
}

std::string expand_context(const json& context_block, const std::vector<json>& messages) {
    std::string filler = context_block.value("filler", "");
    int target_tokens = context_block.value("target_tokens", 2000);
    std::string position = context_block.value("position", "end");
    if (filler.empty()) return "";

    size_t target_chars = static_cast<size_t>(target_tokens) * 4;
    size_t filler_len = filler.size();
    if (filler_len == 0) return "";

    std::string expanded;
    expanded.reserve(target_chars);
    for (size_t i = 0, reps = (target_chars + filler_len - 1) / filler_len; i < reps; ++i)
        expanded += filler;

    std::string question = extract_user_content(messages);
    if (position == "start") return question + "\n\n" + expanded;
    if (position == "middle") {
        size_t mid = expanded.size() / 2;
        return expanded.substr(0, mid) + "\n\n" + question + "\n\n" + expanded.substr(mid);
    }
    return expanded + "\n\n" + question;
}

static std::vector<BenchScenario> parse_scenario_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Warning: Could not open scenario file: " << path << std::endl;
        return {};
    }
    json data;
    try { data = json::parse(file); }
    catch (const json::exception& e) {
        std::cerr << "Warning: Failed to parse scenario file " << path << ": " << e.what() << std::endl;
        return {};
    }
    if (!data.contains("scenarios") || !data["scenarios"].is_array()) {
        std::cerr << "Warning: No 'scenarios' array in " << path << std::endl;
        return {};
    }

    std::vector<BenchScenario> scenarios;
    for (const auto& item : data["scenarios"]) {
        BenchScenario scenario;
        if (!item.contains("name") || !item["name"].is_string()) continue;
        scenario.source_file = path;
        scenario.name = item["name"].get<std::string>();
        scenario.category = item.value("category", "general");
        scenario.measurement_runs = item.value("measurement_runs", 3);

        if (scenario.category == "embed") {
            if (item.contains("input")) scenario.input = item["input"].get<json>();
        } else if (scenario.category == "imagegen") {
            scenario.warmup_runs = 0;
            if (item.contains("prompt")) scenario.imgconfig = item.get<json>();
        } else if (scenario.category == "vision") {
            scenario.warmup_runs = 0;

            if (!item.contains("image_path") || !item["image_path"].is_string() ||
                item["image_path"].get<std::string>().empty()) {
                throw std::runtime_error("Vision scenario '" + scenario.name +
                                        "' must define a non-empty image_path");
            }
            scenario.image_path = item["image_path"].get<std::string>();

            if (item.contains("messages") && item["messages"].is_array())
                scenario.messages = item["messages"].get<std::vector<json>>();

            scenario.max_tokens = item.value("max_tokens", 1024);
        } else {
            if (item.contains("messages") && item["messages"].is_array())
                scenario.messages = item["messages"].get<std::vector<json>>();
            scenario.max_tokens = item.value("max_tokens", 128);
            scenario.warmup_runs = item.value("warmup_runs", 0);
            if (item.contains("context") && item["context"].is_object()) {
                std::string expanded = expand_context(item["context"], scenario.messages);
                for (auto& msg : scenario.messages) {
                    if (msg.contains("role") && msg["role"] == "user" &&
                        msg.contains("content") && msg["content"].is_string()) {
                        msg["content"] = expanded;
                        break;
                    }
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

    std::vector<std::string> entries;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            if (entry.is_regular_file())
                entries.push_back(entry.path().filename().string());
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Could not read scenario directory " << path << ": " << e.what() << std::endl;
        return {};
    }
    std::sort(entries.begin(), entries.end());

    for (const auto& entry : entries) {
        if (entry.size() < 5 || entry.substr(entry.size() - 5) != ".json") continue;
        auto scenarios = parse_scenario_file(path + "/" + entry);
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
    std::string path = lemon::utils::get_resource_path("resources/bench_scenarios.json");
    std::ifstream test(path);
    if (test.good()) return path;
    return "bench_scenarios.json";
}

static bool scenario_matches_filter(const BenchScenario& s, const std::string& token) {
    if (token == "all") return true;
    if (s.name == token) return true;
    return s.category == token;
}

std::vector<BenchScenario> filter_scenarios(const std::vector<BenchScenario>& all,
                                            const std::vector<std::string>& tokens) {
    if (tokens.empty()) return all;
    std::vector<BenchScenario> filtered;
    for (const auto& s : all) {
        for (const auto& token : tokens) {
            if (scenario_matches_filter(s, token)) {
                filtered.push_back(s);
                break;
            }
        }
    }
    return filtered;
}

static std::vector<BenchScenario> exclude_category(const std::vector<BenchScenario>& all,
                                                    const std::string& category) {
    std::vector<BenchScenario> filtered;
    for (const auto& s : all)
        if (s.category != category) filtered.push_back(s);
    return filtered;
}

static bool backend_supports_capability(const std::string& recipe,
                                        const std::string& /*backend_name*/,
                                        const lemon::ModelType required_type) {
    return lemon::backends::has_backend(recipe) &&
           lemon::backends::backend_serves_mode(recipe, required_type);
}

static lemon::ModelType get_required_model_type_for_scenario(const std::string& category) {
    if (category == "embed") return lemon::ModelType::EMBEDDING;
    if (category == "imagegen") return lemon::ModelType::IMAGE;
    if (category == "transcription") return lemon::ModelType::TRANSCRIPTION;
    if (category == "tts") return lemon::ModelType::TTS;
    if (category == "audio-generation") return lemon::ModelType::AUDIO_GENERATION;
    return lemon::ModelType::LLM;
}

static bool model_has_embeddings_label(const json& model_info) {
    if (!model_info.contains("labels") || !model_info["labels"].is_array()) return false;
    for (const auto& label : model_info["labels"]) {
        if (label.is_string()) {
            std::string s = label.get<std::string>();
            if (s == "embed" || s == "embedding" || s == "embeddings") return true;
        }
    }
    return false;
}

std::vector<BackendDiscovery> discover_backends(const json& sys_info,
                                                const std::vector<std::string>& requested,
                                                const json& model_info) {
    std::vector<BackendDiscovery> result;
    if (!sys_info.contains("recipes") || !sys_info["recipes"].is_object()) {
        std::cerr << "Warning: No recipes found in system-info" << std::endl;
        return result;
    }

    std::string model_recipe;
    if (model_info.contains("recipe") && model_info["recipe"].is_string())
        model_recipe = model_info["recipe"].get<std::string>();

    for (const auto& [recipe_name, recipe_data] : sys_info["recipes"].items()) {
        if (!model_recipe.empty() && recipe_name != model_recipe) continue;
        if (!recipe_data.contains("backends") || !recipe_data["backends"].is_object()) continue;
        for (const auto& [backend_name, backend_data] : recipe_data["backends"].items()) {
            std::string state = backend_data.value("state", "unknown");
            if (state != "installed" && state != "update_required" && state != "update_available") continue;
            if (!requested.empty()) {
                bool found = false;
                for (const auto& req : requested)
                    if (req == backend_name) { found = true; break; }
                if (!found) continue;
            }
            result.push_back({recipe_name, backend_name});
        }
    }
    return result;
}

static const std::map<std::string, std::string> RECIPE_ARGS_KEY = {
    {"llamacpp", "llamacpp_args"},
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
        if (const auto* desc = lemon::backends::descriptor_for(recipe);
            desc && desc->selectable_backend) {
            request_body[desc->effective_config_section() + "_backend"] = backend;
        }
        if (ctx_size > 0) request_body["ctx_size"] = ctx_size;
        if (!backend_args.empty()) {
            auto it = RECIPE_ARGS_KEY.find(recipe);
            if (it != RECIPE_ARGS_KEY.end()) {
                request_body[it->second] = backend_args;
                request_body["merge_args"] = false;
            }
        }

        BenchBackendResult tmp;
        tmp.recipe = recipe;
        tmp.backend = backend;
        tmp.ctx_size = ctx_size;
        tmp.backend_args = backend_args;
        std::cout << "  Loading model with " << tmp.label() << "..." << std::flush;
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

static void extract_usage_into_result(const json& usage, BenchRunResult& result) {
    if (usage.contains("prompt_tokens")) result.input_tokens = usage["prompt_tokens"].get<int>();
    if (usage.contains("completion_tokens")) result.output_tokens = usage["completion_tokens"].get<int>();
    if (usage.contains("prefill_duration_ttft")) result.ttft_ms = usage["prefill_duration_ttft"].get<double>() * 1000.0;
    if (usage.contains("decoding_speed_tps")) result.tps = usage["decoding_speed_tps"].get<double>();
}

static void extract_timings_into_result(const json& timings, BenchRunResult& result) {
    if (timings.contains("prompt_ms")) result.ttft_ms = timings["prompt_ms"].get<double>();
    if (timings.contains("predicted_per_second")) result.tps = timings["predicted_per_second"].get<double>();
    if (timings.contains("prompt_n")) result.input_tokens = timings["prompt_n"].get<int>();
    if (timings.contains("predicted_n")) result.output_tokens = timings["predicted_n"].get<int>();
}

static void extract_mem_use_into_result(lemonade::LemonadeClient& client, BenchRunResult& result) {
    double vram = -1.0, mem = -1.0;
    try {
        std::string resp = client.make_request("/api/v1/system-stats", "GET", "", "", 5000, 5000);
        auto stats = json::parse(resp);
        if (stats.contains("vram_gb") && stats["vram_gb"].is_number()) vram = stats["vram_gb"].get<double>();
        if (stats.contains("memory_gb") && stats["memory_gb"].is_number()) mem = stats["memory_gb"].get<double>();
    } catch (...) {}
    result.vram_gb = vram;
    result.memory_gb = mem;
}

static void compute_tps_from_tokens_and_time(BenchRunResult& result) {
    if (result.tps <= 0 && result.output_tokens > 0 && result.total_time_ms > 0)
        result.tps = (result.output_tokens * 1000.0) / result.total_time_ms;
}

struct BenchStrategy {
    std::string endpoint;
    std::function<json()> build_request;
    std::function<void(const json&, BenchRunResult&, bool)> parse_response;
};

static BenchRunResult run_bench_with_strategy(lemonade::LemonadeClient& client,
                                                bool memory_tracking,
                                                bool capture_response,
                                                int timeout,
                                                const BenchStrategy& strategy) {
    BenchRunResult result;
    result.success = false;

    if (memory_tracking) {
        // Warmup memory stats query (discard)
        try {
            client.make_request("/api/v1/system-stats", "GET", "", "", 5000, 5000);
        } catch (...) {}
    }

    json request_body = strategy.build_request();
    auto start = steady_clock::now();

    try {
        std::string response = client.make_request(strategy.endpoint, "POST",
                                                    request_body.dump(), "application/json",
                                                    timeout, timeout);
        auto resp_json = json::parse(response);
        strategy.parse_response(resp_json, result, capture_response);
    } catch (const std::exception& e) {
        std::cerr << "    Benchmark run failed: " << e.what() << std::endl;
        return result;
    }

    auto end = steady_clock::now();
    result.total_time_ms = duration<double, std::milli>(end - start).count();
    compute_tps_from_tokens_and_time(result);

    if (memory_tracking)
        extract_mem_use_into_result(client, result);

    if (!result.valid) {
        std::cerr << "    Benchmark run failed (no meaningful data extracted)" << std::endl;
        return result;
    }

    result.success = true;
    return result;
}

static BenchStrategy make_textgen_strategy(const std::string& model, const BenchScenario& scenario) {
    return BenchStrategy{
        "/api/v1/chat/completions",
        [&model, &scenario]() -> json {
            std::vector<json> messages = scenario.messages;
            if (is_vision_scenario(scenario)) {
                for (auto& msg : messages) {
                    if (msg.contains("role") && msg["role"] == "user") {
                        embed_image_into_message(msg, scenario);
                        break;
                    }
                }
            }
            json body;
            body["model"] = model;
            body["messages"] = std::move(messages);
            body["max_completion_tokens"] = scenario.max_tokens;
            body["temperature"] = 0;
            return body;
        },
        [](const json& resp, BenchRunResult& result, bool capture_response) {
            if (capture_response) {
                if (resp.contains("output")) {
                    result.response_text = resp["output"].dump();
                } else if (resp.contains("choices") && resp["choices"].is_array() &&
                           !resp["choices"].empty()) {
                    const auto& choice = resp["choices"][0];
                    if (choice.contains("message") && choice["message"].contains("content")) {
                        const auto& content = choice["message"]["content"];
                        result.response_text = content.is_string() ? content.get<std::string>() : content.dump();
                    } else {
                        result.response_text = choice.dump();
                    }
                } else {
                    result.response_text = "null";
                }
            }
            if (resp.contains("timings") && resp["timings"].is_object()) {
                extract_timings_into_result(resp["timings"], result);
            }
            if (resp.contains("usage") && resp["usage"].is_object()) {
                extract_usage_into_result(resp["usage"], result);
            }

            result.valid = !(result.ttft_ms <= 0 && result.tps <= 0 && result.input_tokens <= 0 && result.output_tokens <= 0);
        }
    };
}

static BenchStrategy make_embed_strategy(const std::string& model, const BenchScenario& scenario) {
    return BenchStrategy{
        "/api/v1/embeddings",
        [&model, &scenario]() -> json {
            json body;
            body["model"] = model;
            body["input"] = scenario.input;
            return body;
        },
        [](const json& resp, BenchRunResult& result, bool capture_response) {
            if (capture_response) {
                if (resp.contains("data") && !resp["data"].empty())
                    result.response_text = resp["data"].dump();
                else
                    result.response_text = "null";
            }
            if (resp.contains("timings") && resp["timings"].is_object()) {
                extract_timings_into_result(resp["timings"], result);
            }
            if (resp.contains("usage") && resp["usage"].is_object()) {
                result.input_tokens = resp["usage"]["prompt_tokens"].get<int>();
                result.output_tokens = resp["usage"]["total_tokens"].get<int>();
            }

            result.valid = true;
        }
    };
}

static BenchStrategy make_imagegen_strategy(const std::string& model, const BenchScenario& scenario,
                                             bool capture_response) {
    return BenchStrategy{
        "/api/v1/images/generations",
        [&scenario, capture_response, model]() -> json {
            json body;
            body["model"] = model;
            if (!scenario.imgconfig.contains("prompt") || !scenario.imgconfig["prompt"].is_string()) {
                // Will fail with a clear error message
                body["prompt"] = "";
            } else {
                body["prompt"] = scenario.imgconfig.value("prompt", "");
            }
            if (capture_response) body["response_format"] = "b64_json";
            if (scenario.imgconfig.contains("size") && scenario.imgconfig["size"].is_string())
                body["size"] = scenario.imgconfig.value("size", "256x256");
            if (scenario.imgconfig.contains("steps") && scenario.imgconfig["steps"].is_number_integer())
                body["steps"] = scenario.imgconfig.value("steps", 1);
            if (scenario.imgconfig.contains("cfg_scale") && scenario.imgconfig["cfg_scale"].is_number())
                body["cfg_scale"] = scenario.imgconfig.value("cfg_scale", 1.0);
            if (scenario.imgconfig.contains("seed") && scenario.imgconfig["seed"].is_number_integer())
                body["seed"] = scenario.imgconfig.value("seed", 42);
            else
                body["seed"] = 42;
            return body;
        },
        [](const json& resp, BenchRunResult& result, bool capture_response) {
            result.response_text = "null";
            if (capture_response && resp.contains("data") && !resp["data"].empty()) {
                const auto& data = resp["data"];
                if (data.is_array() && !data.empty()) {
                    const auto& first = data[0];
                    if (first.contains("b64_json"))
                        result.response_text = first["b64_json"].get<std::string>();
                }
            }
            if (resp.contains("timings") && resp["timings"].is_object()) {
                extract_timings_into_result(resp["timings"], result);
            }
            if (resp.contains("data") && resp["data"].is_array() && !resp["data"].empty()) {
                result.valid = true;
            }
        }
    };
}

// Public dispatchers (kept for API compatibility with header declarations)
BenchRunResult run_single_bench(lemonade::LemonadeClient& client,
                                const std::string& model,
                                const BenchScenario& scenario,
                                bool memory_tracking,
                                bool capture_response,
                                int timeout) {
    if (scenario.category == "embed")
        return run_single_bench_embed(client, model, scenario, memory_tracking, capture_response, timeout);
    if (scenario.category == "imagegen")
        return run_single_bench_imagegen(client, model, scenario, memory_tracking, capture_response, timeout);
    return run_single_bench_textgen(client, model, scenario, memory_tracking, capture_response, timeout);
}

BenchRunResult run_single_bench_textgen(lemonade::LemonadeClient& client,
                                        const std::string& model,
                                        const BenchScenario& scenario,
                                        bool memory_tracking,
                                        bool capture_response,
                                        int timeout) {
    return run_bench_with_strategy(client, memory_tracking, capture_response, timeout, make_textgen_strategy(model, scenario));
}

BenchRunResult run_single_bench_embed(lemonade::LemonadeClient& client,
                                      const std::string& model,
                                      const BenchScenario& scenario,
                                      bool memory_tracking,
                                      bool capture_response,
                                      int timeout) {
    auto strategy = make_embed_strategy(model, scenario);
    auto result = run_bench_with_strategy(client, memory_tracking, capture_response, timeout, strategy);
    if (result.success) {
        // Embedding benchmarks: ttft_ms is the total wall-clock time
        result.ttft_ms = result.total_time_ms;
    }
    return result;
}

BenchRunResult run_single_bench_imagegen(lemonade::LemonadeClient& client,
                                         const std::string& model,
                                         const BenchScenario& scenario,
                                         bool memory_tracking,
                                         bool capture_response,
                                         int timeout) {
    if (!scenario.imgconfig.contains("prompt") || !scenario.imgconfig["prompt"].is_string()) {
        std::cerr << "    Image generation scenario missing 'prompt' field." << std::endl;
        BenchRunResult result;
        result.success = false;
        return result;
    }
    auto strategy = make_imagegen_strategy(model, scenario, capture_response);
    auto result = run_bench_with_strategy(client, memory_tracking, capture_response, timeout, strategy);
    if (result.success) {
        result.ttft_ms = result.total_time_ms;
    }
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
                                 int timeout,
                                 const std::string& backend_args,
                                 const std::string& response_log_path,
                                 const std::string& response_timestamp) {
    BenchScenarioResult result;
    result.scenario_name = scenario.name;
    result.category = scenario.category;

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
        run_single_bench(client, model, scenario, false, false, timeout);
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
        auto run_result = run_single_bench(client, model, scenario, memory_tracking,
                                            !response_log_path.empty(), timeout);
        if (!run_result.success) {
            result.failed_runs++;
            std::cout << " FAILED (excluded from stats)" << std::endl;
            continue;
        }

        if (!response_log_path.empty()) {
            std::ofstream log(response_log_path, std::ios::app);
            if (log.is_open()) {
                json entry;
                entry["timestamp"] = response_timestamp;
                entry["model"] = model;
                entry["recipe"] = recipe;
                entry["backend"] = backend;
                entry["ctx_size"] = ctx_size;
                entry["backend_args"] = backend_args;
                entry["scenario"] = scenario.name;
                entry["category"] = scenario.category;
                entry["run_number"] = i + 1;
                entry["input_tokens"] = run_result.input_tokens;
                entry["output_tokens"] = run_result.output_tokens;
                entry["ttft_ms"] = run_result.ttft_ms;
                entry["tps"] = run_result.tps;
                entry["response"] = run_result.response_text;
                log << entry.dump() << "\n";
            } else {
                std::cerr << "\nWarning: Could not open response log: " << response_log_path << std::endl;
            }
            run_result.response_text.clear();
            run_result.response_text.shrink_to_fit();
        }

        std::cout << " TTFT=" << std::fixed << std::setprecision(1) << run_result.ttft_ms << "ms"
                  << " TPS=" << std::fixed << std::setprecision(1) << run_result.tps << std::endl;
        result.runs.push_back(run_result);
    }

    return result;
}

static std::map<std::string, json> preflight_models(lemonade::LemonadeClient& client,
                                                     const std::vector<std::string>& models,
                                                     bool auto_pull) {
    std::map<std::string, json> info_by_name;
    for (const auto& model : models) {
        json model_info = client.get_model_info(model);

        if (model_info.empty()) {
            if (!auto_pull) {
                std::cerr << "Error: Model '" << model << "' not found." << std::endl;
                std::cerr << "Use --auto-pull to download missing models before benchmarking." << std::endl;
                return {};
            }
            std::cout << "Preflight: model '" << model << "' not found. Auto-pulling..." << std::endl;
            json pull_req;
            pull_req["model_name"] = model;
            if (client.pull_model(pull_req) != 0) {
                std::cerr << "Error: Failed to pull model '" << model << "'." << std::endl;
                return {};
            }
            model_info = client.get_model_info(model);
            if (model_info.empty()) {
                std::cerr << "Error: Model '" << model << "' still unavailable after auto-pull." << std::endl;
                return {};
            }
        }

        if (model_info.contains("downloaded") && !model_info["downloaded"].get<bool>()) {
            if (!auto_pull) {
                std::cerr << "Error: Model '" << model << "' is not downloaded." << std::endl;
                std::cerr << "Run 'lemonade pull " << model << "' first, or use --auto-pull." << std::endl;
                return {};
            }
            std::cout << "Preflight: model '" << model << "' is not downloaded. Auto-pulling..." << std::endl;
            json pull_req;
            pull_req["model_name"] = model;
            if (client.pull_model(pull_req) != 0) {
                std::cerr << "Error: Failed to pull model '" << model << "'." << std::endl;
                return {};
            }
            model_info = client.get_model_info(model);
            if (model_info.empty() || (model_info.contains("downloaded") && !model_info["downloaded"].get<bool>())) {
                std::cerr << "Error: Model '" << model << "' is not downloaded after auto-pull." << std::endl;
                return {};
            }
        }

        info_by_name[model] = std::move(model_info);
    }
    return info_by_name;
}

struct ModelSetSummary {
    std::vector<std::string> current_models;
    std::vector<std::string> previous_models;
    std::vector<std::string> matched_models;
    std::vector<std::string> new_models;
    std::vector<std::string> removed_models;
};

static ModelSetSummary compute_model_set_summary(
        const std::vector<std::string>& current_models,
        const std::map<std::string, json>& previous_by_model) {
    ModelSetSummary summary;
    summary.current_models = current_models;

    summary.previous_models.reserve(previous_by_model.size());
    for (const auto& [name, _] : previous_by_model)
        summary.previous_models.push_back(name);

    for (const auto& name : current_models) {
        if (previous_by_model.find(name) != previous_by_model.end())
            summary.matched_models.push_back(name);
        else
            summary.new_models.push_back(name);
    }
    for (const auto& name : summary.previous_models) {
        if (std::find(current_models.begin(), current_models.end(), name) == current_models.end())
            summary.removed_models.push_back(name);
    }
    return summary;
}

int handle_bench_command(lemonade::LemonadeClient& client, const BenchConfig& config) {
    if (config.models.empty()) {
        std::cerr << "Error: At least one model must be provided." << std::endl;
        return 1;
    }

    // Deduplicate models while preserving order
    std::vector<std::string> unique_models;
    std::unordered_set<std::string> seen_models;
    for (const auto& model : config.models) {
        if (seen_models.find(model) == seen_models.end()) {
            unique_models.push_back(model);
            seen_models.insert(model);
        }
    }
    if (unique_models.size() < config.models.size()) {
        std::cout << "Note: Removed " << (config.models.size() - unique_models.size())
                  << " duplicate model(s). Testing " << unique_models.size()
                  << " unique model(s)." << std::endl;
    }

    const std::string command_timestamp = get_timestamp_iso();

    if (!config.response_log.empty()) {
        try {
            std::filesystem::path log_path(config.response_log);
            if (!log_path.parent_path().empty())
                std::filesystem::create_directories(log_path.parent_path());
        } catch (const std::exception& e) {
            std::cerr << "Warning: Could not prepare response log path: " << e.what() << std::endl;
        }
    }

    // Preflight: ensure all models are available
    auto model_info_by_name = preflight_models(client, unique_models, config.auto_pull);
    if (model_info_by_name.empty()) return 1;

    // Load scenarios (shared across models)
    std::vector<BenchScenario> scenarios;
    if (!config.scenario_file.empty()) {
        scenarios = load_scenarios_from_file(config.scenario_file);
    } else if (!config.scenario_dir.empty()) {
        scenarios = load_scenarios_from_dir(config.scenario_dir);
    } else {
        scenarios = load_scenarios_from_file(resolve_default_scenario_file());
    }
    if (scenarios.empty()) {
        std::cerr << "Error: No scenarios loaded." << std::endl;
        return 1;
    }

    // Filter scenarios
    if (config.scenario_names.empty()) {
        scenarios = exclude_category(scenarios, "long-context");
        scenarios = exclude_category(scenarios, "embed");
    } else {
        scenarios = filter_scenarios(scenarios, config.scenario_names);
    }
    if (scenarios.empty()) {
        std::cerr << "Error: No scenarios matched the filter." << std::endl;
        return 1;
    }

    // Fetch system-info and discover backends
    json sys_info;
    try {
        std::string resp = client.make_request("/api/v1/system-info", "GET", "", "", 10000, 10000);
        sys_info = json::parse(resp);
    } catch (const std::exception& e) {
        std::cerr << "Error: Failed to fetch system-info: " << e.what() << std::endl;
        return 1;
    }

    // Fetch version from /health (not present in system-info)
    std::string lemonade_version;
    try {
        std::string resp = client.make_request("/api/v1/health", "GET", "", "", 5000, 5000);
        json health = json::parse(resp);
        if (health.contains("version") && health["version"].is_string()) {
            lemonade_version = health["version"].get<std::string>();
        }
    } catch (const std::exception& e) {
        std::cerr << "Warning: Failed to fetch lemonade version from /health: " << e.what() << std::endl;
    }

    std::unordered_set<std::string> test_recipes, test_backends;
    std::map<std::string, std::vector<BackendDiscovery>> backends_by_model;
    for (const auto& model : unique_models) {
        const auto it = model_info_by_name.find(model);
        const json& model_info = (it != model_info_by_name.end()) ? it->second : json::object();
        auto backends = discover_backends(sys_info, config.backends, model_info);
        backends_by_model[model] = backends;
        for (const auto& [recipe, backend] : backends) {
            test_recipes.insert(recipe);
            test_backends.insert(backend);
        }
    }
    json hardware_profile = build_hardware_profile(
        sys_info,
        {test_recipes.begin(), test_recipes.end()},
        {test_backends.begin(), test_backends.end()},
        lemonade_version);

    // Execute benchmarks for each model
    struct ModelBenchResult {
        std::string model;
        std::string timestamp;
        std::vector<BenchBackendResult> results;
    };
    std::vector<ModelBenchResult> by_model;

    for (const auto& model : unique_models) {
        const auto it = model_info_by_name.find(model);
        if (it == model_info_by_name.end()) {
            std::cerr << "Error: Missing preflight model info for '" << model << "'." << std::endl;
            return 1;
        }
        const json& model_info = it->second;
        const std::string model_timestamp = get_timestamp_iso();

        auto backends = backends_by_model[model];
        if (backends.empty()) {
            std::cerr << "Error: No suitable backends found for model '" << model << "'." << std::endl;
            return 1;
        }

        // Determine context sizes
        std::vector<int> ctx_sizes = config.ctx_sizes;
        if (ctx_sizes.empty()) {
            int default_ctx = 4096;
            if (model_info.contains("recipe_options") && model_info["recipe_options"].is_object()) {
                auto& opts = model_info["recipe_options"];
                if (opts.contains("ctx_size") && opts["ctx_size"].is_number())
                    default_ctx = opts["ctx_size"].get<int>();
            }
            ctx_sizes = {default_ctx};
        }

        std::vector<BenchBackendResult> all_results;

        for (const auto& [recipe, backend] : backends) {
            std::vector<std::string> recipe_args_list;
            auto args_it = config.backend_args.find(recipe);
            if (args_it != config.backend_args.end() && !args_it->second.empty())
                recipe_args_list = args_it->second;
            else
                recipe_args_list = {""};

            for (int ctx_size : ctx_sizes) {
                for (const auto& recipe_args : recipe_args_list) {
                    BenchBackendResult tmp;
                    tmp.recipe = recipe;
                    tmp.backend = backend;
                    tmp.ctx_size = ctx_size;
                    tmp.backend_args = recipe_args;
                    std::cout << "\n=== [" << model << "] " << tmp.label() << " ===" << std::endl;

                    BenchBackendResult backend_result;
                    backend_result.recipe = recipe;
                    backend_result.backend = backend;
                    backend_result.ctx_size = ctx_size;
                    backend_result.backend_args = recipe_args;

                    for (const auto& scenario : scenarios) {
                        std::cout << "  Scenario: " << scenario.name << " (" << scenario.category << ")" << std::endl;

                        // Filter backends based on scenario category
                        bool backend_suitable = false;
                        const auto required_type = get_required_model_type_for_scenario(scenario.category);
                        if (scenario.category == "embed") {
                            backend_suitable = model_has_embeddings_label(model_info);
                        } else if (scenario.category == "imagegen") {
                            backend_suitable = backend_supports_capability(recipe, backend, lemon::ModelType::IMAGE);
                        } else if (scenario.category == "vision") {
                            backend_suitable = model_has_vision_label(model_info) &&
                                               backend_supports_capability(recipe, backend, lemon::ModelType::LLM);
                        } else {
                            backend_suitable = backend_supports_capability(recipe, backend, required_type);
                            if (model_has_embeddings_label(model_info))
                                backend_suitable = false;
                        }

                        if (!backend_suitable) {
                            std::cout << "    Skipping backend/model - not suitable for " << scenario.category << " scenario" << std::endl;
                            continue;
                        }

                        int warmup = scenario.warmup_runs;
                        int runs = scenario.measurement_runs;
                        if (config.warmup_runs > 0) warmup = config.warmup_runs;
                        if (config.measurement_runs > 0) runs = config.measurement_runs;

                        auto scenario_result = run_scenario(client, model, scenario, warmup, runs,
                                                            config.memory_tracking, config.reload,
                                                            recipe, backend, ctx_size, config.timeout, recipe_args,
                                                            config.response_log, command_timestamp);
                        backend_result.scenarios.push_back(scenario_result);
                    }

                    if (!backend_result.scenarios.empty())
                        all_results.push_back(backend_result);
                }
            }
        }

        if (all_results.empty()) {
            std::cerr << "Error: No benchmark results for model '" << model << "' (all backends failed to load)." << std::endl;
            return 1;
        }

        by_model.push_back({model, model_timestamp, std::move(all_results)});
    }

    // Output results
    if (config.compare_file.empty()) {
        // Normal output
        if (config.json_output) {
            json output;
            output["timestamp"] = command_timestamp;
            output["hardware"] = hardware_profile;
            output["models"] = json::array();
            for (const auto& mr : by_model)
                output["models"].push_back(to_json(mr.results, mr.model, mr.timestamp, config));

            std::string json_str = output.dump(2);
            if (!config.output_file.empty()) {
                std::ofstream out(config.output_file);
                if (!out.is_open()) {
                    std::cerr << "Error: Could not write to " << config.output_file << std::endl;
                    return 1;
                }
                out << json_str;
                std::cout << "Results written to " << config.output_file << std::endl;
            } else {
                std::cout << json_str << std::endl;
            }
        } else {
            for (const auto& mr : by_model)
                print_table(mr.results, mr.model, config.measurement_runs >= 10);

            if (!config.output_file.empty()) {
                json output;
                output["timestamp"] = command_timestamp;
                output["hardware"] = hardware_profile;
                output["models"] = json::array();
                for (const auto& mr : by_model)
                    output["models"].push_back(to_json(mr.results, mr.model, mr.timestamp, config));
                std::ofstream out(config.output_file);
                if (out.is_open()) {
                    out << output.dump(2);
                    std::cout << "JSON results also written to " << config.output_file << std::endl;
                } else {
                    std::cerr << "Warning: Could not write to " << config.output_file << std::endl;
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

        auto previous_by_model = extract_models_from_results(previous_results);
        std::string previous_timestamp = extract_results_timestamp(previous_results);

        std::vector<std::string> current_models;
        current_models.reserve(by_model.size());
        for (const auto& mr : by_model) current_models.push_back(mr.model);

        auto summary = compute_model_set_summary(current_models, previous_by_model);

        struct ModelComparisonResult {
            std::string model;
            std::string timestamp;
            json previous_for_model;
            std::vector<BenchComparisonDelta> deltas;
            const std::vector<BenchBackendResult>* results = nullptr;
        };
        std::vector<ModelComparisonResult> comparison_results;
        comparison_results.reserve(by_model.size());

        for (const auto& mr : by_model) {
            json prev_for_model = json::object();
            auto it = previous_by_model.find(mr.model);
            if (it != previous_by_model.end()) prev_for_model = it->second;
            comparison_results.emplace_back(ModelComparisonResult{
                mr.model, mr.timestamp, prev_for_model,
                compute_deltas(mr.results, prev_for_model), &mr.results
            });
        }

        if (config.json_output) {
            json output;
            output["timestamp"] = command_timestamp;
            output["hardware"] = hardware_profile;
            output["compare_file"] = config.compare_file;
            if (!previous_timestamp.empty()) output["previous_timestamp"] = previous_timestamp;
            output["model_summary"] = {
                {"current_models", summary.current_models},
                {"previous_models", summary.previous_models},
                {"matched_models", summary.matched_models},
                {"new_models", summary.new_models},
                {"removed_models", summary.removed_models}
            };
            output["models"] = json::array();
            for (const auto& c : comparison_results)
                output["models"].push_back(build_comparison_json(*c.results, c.model, c.timestamp,
                                                                  config, c.previous_for_model, c.deltas));

            std::string json_str = output.dump(2);
            if (!config.output_file.empty()) {
                std::ofstream out(config.output_file);
                if (!out.is_open()) {
                    std::cerr << "Error: Could not write to " << config.output_file << std::endl;
                    return 1;
                }
                out << json_str;
                std::cout << "Results written to " << config.output_file << std::endl;
            } else {
                std::cout << json_str << std::endl;
            }
        } else {
            std::cout << "\nModel set comparison" << std::endl;
            std::cout << std::string(100, '=') << std::endl;
            std::cout << "Current models (" << summary.current_models.size() << "):  "
                      << join_list(summary.current_models) << std::endl;
            std::cout << "Previous models (" << summary.previous_models.size() << "): "
                      << join_list(summary.previous_models) << std::endl;
            std::cout << "Matched: " << join_list(summary.matched_models) << std::endl;
            std::cout << "New:     " << join_list(summary.new_models) << std::endl;
            std::cout << "Removed: " << join_list(summary.removed_models) << std::endl;

            for (const auto& c : comparison_results) {
                print_table(*c.results, c.model, config.measurement_runs >= 10);
                print_comparison(c.deltas, c.model, config.compare_file, previous_timestamp);
            }
        }
    }

    unload_all_models(client);
    return 0;
}

CLI::App* register_bench_command(CLI::App& parent,
                                 std::string& output_file,
                                 BenchCliOptions& opts) {
    CLI::App* cmd = parent.add_subcommand("bench", "Benchmark model performance for different model types")
        ->group("Model management");
    cmd->add_option("models", opts.models, "One or more model names to benchmark")
        ->required()->type_name("MODEL")->expected(1, -1);
    cmd->add_option("--backend", opts.backends, "Backend to test (e.g., vulkan, metal, cpu). Repeat for multiple.")
        ->type_name("BACKEND")->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--ctx-size", opts.ctx_sizes, "Context size to test. Repeat for multiple sizes.")
        ->type_name("SIZE")->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--runs", opts.runs, "Number of measurement runs per scenario (default: 3)")->type_name("N");
    cmd->add_option("--warmup", opts.warmup, "Number of warmup runs per scenario (default: 0)")->type_name("N");
    cmd->add_option("--scenarios", opts.scenario_names,
        "Scenario name(s) or category to run (e.g. chat, coding, long-context). "
        "Use 'all' to include every scenario including long-context. "
        "Default: all scenarios except long-context.")
        ->type_name("NAME|CATEGORY")->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--scenario-file", opts.scenario_file, "Load scenarios from a single JSON file")->type_name("FILE");
    cmd->add_option("--scenario-dir", opts.scenario_dir, "Load all .json scenario files from a directory")->type_name("DIR");
    cmd->add_flag("--json", opts.json_output, "Output results as JSON");
    cmd->add_option("--output", output_file, "Write results to file (in addition to stdout)")->type_name("FILE");
    cmd->add_option("--compare", opts.compare_file, "Compare results against a previously saved JSON file")->type_name("FILE");
    cmd->add_flag("--auto-pull", opts.auto_pull, "Automatically pull the model if not downloaded");
    cmd->add_flag("--no-memory", opts.no_memory, "Disable VRAM/RAM tracking");
    cmd->add_flag("--no-reload", opts.no_reload, "Skip model reload between scenarios (faster but prompt cache may skew results)");
    cmd->add_option("--response-log", opts.response_log, "Write captured responses to a JSONL logfile")->type_name("FILE");
    cmd->add_option("--llamacpp-args", opts.llamacpp_args, "Custom args for llama-server (e.g. \"-b 2048 -ub 1024\"). Repeat for multiple.")
        ->type_name("ARGS")->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--flm-args", opts.flm_args, "Safe custom args for flm serve. Repeat for multiple.")
        ->type_name("ARGS")->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--vllm-args", opts.vllm_args, "Custom args for vllm-server. Repeat for multiple.")
        ->type_name("ARGS")->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--sdcpp-args", opts.sdcpp_args, "Custom args for sd-server. Repeat for multiple.")
        ->type_name("ARGS")->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--whispercpp-args", opts.whispercpp_args, "Custom args for whisper-server. Repeat for multiple.")
        ->type_name("ARGS")->multi_option_policy(CLI::MultiOptionPolicy::TakeAll);
    cmd->add_option("--timeout", opts.timeout, "Timeout in seconds for individual requests (default: 300)")
        ->type_name("SECONDS");
    return cmd;
}

BenchConfig build_bench_config(const std::string& output_file,
                               const BenchCliOptions& cli) {
    BenchConfig config;
    config.models = cli.models;
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
    config.response_log = cli.response_log;
    if (!cli.llamacpp_args.empty()) config.backend_args["llamacpp"] = cli.llamacpp_args;
    if (!cli.flm_args.empty()) config.backend_args["flm"] = cli.flm_args;
    if (!cli.vllm_args.empty()) config.backend_args["vllm"] = cli.vllm_args;
    if (!cli.sdcpp_args.empty()) config.backend_args["sd-cpp"] = cli.sdcpp_args;
    if (!cli.whispercpp_args.empty()) config.backend_args["whispercpp"] = cli.whispercpp_args;
    config.timeout = cli.timeout * 1000;
    return config;
}

} // namespace lemon_cli
