#pragma once

#include <iomanip>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

namespace lemon_cli {

struct TurnStats {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    double total_seconds = 0.0;
    double ttft_seconds = -1.0;  // negative: unknown
    double decode_tps = 0.0;     // zero: unknown
    double prefill_tps = 0.0;    // zero: unknown
    bool decode_is_overall = false;
};

inline double json_number(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_number()) ? it->get<double>() : 0.0;
}

// Prefill speed comes only from here because `prompt_n` skips prompt-cached tokens.
inline void apply_server_timings(const nlohmann::json& timings, TurnStats& stats) {
    if (!timings.is_object()) return;
    const double prompt_ms = json_number(timings, "prompt_ms");
    if (prompt_ms > 0.0) stats.ttft_seconds = prompt_ms / 1000.0;
    stats.prefill_tps = json_number(timings, "prompt_per_second");
    stats.decode_tps = json_number(timings, "predicted_per_second");
    const int predicted_n = static_cast<int>(json_number(timings, "predicted_n"));
    if (stats.completion_tokens <= 0 && predicted_n > 0) stats.completion_tokens = predicted_n;
}

// FastFlowLM and RyzenAI report speed inside `usage`.
inline void apply_usage(const nlohmann::json& usage, TurnStats& stats) {
    if (!usage.is_object()) return;
    stats.prompt_tokens = static_cast<int>(json_number(usage, "prompt_tokens"));
    stats.completion_tokens = static_cast<int>(json_number(usage, "completion_tokens"));
    const double ttft = json_number(usage, "prefill_duration_ttft");
    if (ttft > 0.0) stats.ttft_seconds = ttft;
    const double tps = json_number(usage, "decoding_speed_tps");
    if (tps > 0.0) stats.decode_tps = tps;
}

// A negative `client_ttft_seconds` means the turn was not streamed.
inline void finalize_turn_stats(TurnStats& stats, double client_ttft_seconds) {
    if (client_ttft_seconds >= 0.0) stats.ttft_seconds = client_ttft_seconds;

    if (stats.decode_tps > 0.0) return;

    // Tokens after the first are decoded between the first token and the end.
    if (client_ttft_seconds >= 0.0 && stats.completion_tokens >= 2) {
        const double decode_seconds = stats.total_seconds - client_ttft_seconds;
        if (decode_seconds > 0.0) {
            stats.decode_tps = (stats.completion_tokens - 1) / decode_seconds;
            return;
        }
    }
    if (stats.completion_tokens > 0 && stats.total_seconds > 0.0) {
        stats.decode_tps = stats.completion_tokens / stats.total_seconds;
        stats.decode_is_overall = true;
    }
}

inline std::string format_turn_stats(const TurnStats& stats, bool unicode) {
    const char* sep = unicode ? " \xc2\xb7 " : " | ";  // ·
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);

    oss << stats.completion_tokens << " tokens";
    if (stats.decode_tps > 0.0) {
        oss << sep << stats.decode_tps << " tok/s";
        if (stats.decode_is_overall) oss << " (overall)";
    }
    if (stats.ttft_seconds >= 0.0) {
        oss << sep << "TTFT ";
        if (stats.ttft_seconds < 1.0) {
            oss << std::setprecision(0) << stats.ttft_seconds * 1000.0 << " ms";
        } else {
            oss << stats.ttft_seconds << " s";
        }
        oss << std::setprecision(1);
    }
    if (stats.prefill_tps > 0.0) {
        oss << sep << "prefill " << stats.prefill_tps << " tok/s";
    }
    oss << sep << stats.prompt_tokens << " prompt";
    return oss.str();
}

} // namespace lemon_cli
