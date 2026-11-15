#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include <nlohmann/json.hpp>

// The upstream llama.cpp usage is the *entire* prompt, including cached tokens.
// OpenAI clients (including Pi) need the separate cached_tokens count to avoid
// treating cache hits as actual prompt evaluations. Keep unknown distinct from 0.
namespace lemon::hybrid_usage {

using json = nlohmann::json;

inline std::optional<int64_t> nonnegative_integer(const json& object, const char* key) {
    if (!object.is_object() || !object.contains(key) ||
        !object[key].is_number_integer()) return std::nullopt;
    const int64_t value = object[key].get<int64_t>();
    return value >= 0 ? std::optional<int64_t>(value) : std::nullopt;
}

struct Sample {
    int64_t prompt = 0;
    int64_t output = 0;
    std::optional<int64_t> cached;
    std::optional<int64_t> evaluated;
    std::optional<double> prefill_ms;
};

inline Sample extract(const json& response) {
    Sample sample;
    if (!response.is_object()) return sample;
    const json usage = response.value("usage", json::object());
    const json timings = response.value("timings", json::object());
    sample.prompt = nonnegative_integer(usage, "prompt_tokens").value_or(0);
    sample.output = nonnegative_integer(usage, "completion_tokens").value_or(0);
    if (usage.is_object() && usage.contains("prompt_tokens_details") &&
        usage["prompt_tokens_details"].is_object()) {
        sample.cached = nonnegative_integer(usage["prompt_tokens_details"], "cached_tokens");
    }
    if (!sample.cached && usage.is_object() && usage.contains("input_tokens_details") &&
        usage["input_tokens_details"].is_object()) {
        sample.cached = nonnegative_integer(usage["input_tokens_details"], "cached_tokens");
    }
    if (!sample.cached) sample.cached = nonnegative_integer(timings, "cache_n");
    // Reject corrupt or inconsistent counters rather than telling Pi an
    // impossible number of cache hits.
    if (sample.cached && *sample.cached > sample.prompt) sample.cached.reset();
    sample.evaluated = nonnegative_integer(timings, "prompt_n");
    if (!sample.evaluated && sample.cached) {
        sample.evaluated = sample.prompt - *sample.cached;
    }
    if (timings.is_object() && timings.contains("prompt_ms") &&
        timings["prompt_ms"].is_number()) {
        const double ms = timings["prompt_ms"].get<double>();
        if (std::isfinite(ms) && ms >= 0.0) sample.prefill_ms = ms;
    }
    return sample;
}

struct RoleTotals {
    int64_t calls = 0;
    int64_t prompt = 0;
    int64_t output = 0;
    int64_t cached = 0;
    int64_t cache_reports = 0;
    int64_t evaluated = 0;
    int64_t evaluated_reports = 0;
    double prefill_ms = 0.0;
    int64_t prefill_reports = 0;
    double wall_ms = 0.0;

    void add(const Sample& sample, double duration_ms) {
        ++calls;
        prompt += sample.prompt;
        output += sample.output;
        wall_ms += duration_ms;
        if (sample.cached) { cached += *sample.cached; ++cache_reports; }
        if (sample.evaluated) { evaluated += *sample.evaluated; ++evaluated_reports; }
        if (sample.prefill_ms) { prefill_ms += *sample.prefill_ms; ++prefill_reports; }
    }
};

inline json role_diagnostics(const RoleTotals& now, const RoleTotals& before) {
    return {{"calls", now.calls - before.calls},
            {"prompt_tokens", now.prompt - before.prompt},
            {"completion_tokens", now.output - before.output},
            {"cache_samples", now.cache_reports - before.cache_reports},
            {"cached_tokens_observed", now.cached - before.cached},
            {"evaluated_samples", now.evaluated_reports - before.evaluated_reports},
            {"evaluated_tokens_observed", now.evaluated - before.evaluated},
            {"prefill_samples", now.prefill_reports - before.prefill_reports},
            {"prefill_ms_observed", now.prefill_ms - before.prefill_ms},
            {"wall_ms", now.wall_ms - before.wall_ms}};
}

struct Totals {
    int64_t calls = 0;
    int64_t routing_calls = 0;
    int64_t reply_calls = 0;
    int64_t cached = 0;
    int64_t cache_reports = 0;
    int64_t evaluated = 0;
    int64_t evaluated_reports = 0;
    double prefill_ms = 0.0;
    int64_t prefill_reports = 0;
    double routing_wall_ms = 0.0;
    double reply_wall_ms = 0.0;
    RoleTotals orchestrator;
    RoleTotals worker;

    void add(const Sample& sample, bool routing, double wall_ms,
             bool is_orchestrator = true) {
        (is_orchestrator ? orchestrator : worker).add(sample, wall_ms);
        ++calls;
        if (routing) {
            ++routing_calls;
            routing_wall_ms += wall_ms;
        } else {
            ++reply_calls;
            reply_wall_ms += wall_ms;
        }
        if (sample.cached) {
            cached += *sample.cached;
            ++cache_reports;
        }
        if (sample.evaluated) {
            evaluated += *sample.evaluated;
            ++evaluated_reports;
        }
        if (sample.prefill_ms) {
            prefill_ms += *sample.prefill_ms;
            ++prefill_reports;
        }
    }
};

// Attach cache details only when *every* internal call reported them. A
// partial sum would be misleading and could undercount actual prefix reuse.
inline json request_usage(int64_t prompt, int64_t output,
                          const Totals& total, const Totals& before) {
    json usage = {{"prompt_tokens", prompt}, {"completion_tokens", output},
                  {"total_tokens", prompt + output}};
    const int64_t calls = total.calls - before.calls;
    const int64_t reports = total.cache_reports - before.cache_reports;
    const int64_t cached = total.cached - before.cached;
    if (calls > 0 && reports == calls && cached <= prompt) {
        usage["prompt_tokens_details"] = {{"cached_tokens", cached}};
    }
    return usage;
}

inline json request_diagnostics(const Totals& total, const Totals& before) {
    return {{"model_calls", total.calls - before.calls},
            {"routing_calls", total.routing_calls - before.routing_calls},
            {"reply_calls", total.reply_calls - before.reply_calls},
            {"cache_samples", total.cache_reports - before.cache_reports},
            {"cached_tokens_observed", total.cached - before.cached},
            {"prefill_token_samples", total.evaluated_reports - before.evaluated_reports},
            {"evaluated_prompt_tokens_observed", total.evaluated - before.evaluated},
            {"prefill_time_samples", total.prefill_reports - before.prefill_reports},
            {"prefill_ms_observed", total.prefill_ms - before.prefill_ms},
            {"routing_wall_ms", total.routing_wall_ms - before.routing_wall_ms},
            {"reply_wall_ms", total.reply_wall_ms - before.reply_wall_ms},
            {"orchestrator", role_diagnostics(total.orchestrator, before.orchestrator)},
            {"worker", role_diagnostics(total.worker, before.worker)}};
}

} // namespace lemon::hybrid_usage
