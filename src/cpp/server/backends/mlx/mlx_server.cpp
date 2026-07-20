#include "lemon/backends/mlx/mlx_server.h"
#include "lemon/backends/mlx/mlx.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/error_types.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/http_client.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace backends {
namespace {

constexpr const char* kRecipe = "lemon-mlx";
constexpr const char* kLog = "MLX";

bool is_linux_x64() {
#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64))
    return true;
#else
    return false;
#endif
}

bool is_macos_arm64() {
#if defined(__APPLE__) && defined(__aarch64__)
    return true;
#else
    return false;
#endif
}

bool is_supported_rocm_arch(const std::string& arch) {
    // Current lemon-mlx-engine ROCm releases are built and validated on gfx1151.
    // Keep this intentionally narrow until the engine publishes per-arch or
    // verified multi-arch assets.
    return arch == "gfx1151";
}

bool is_mlx_rocm_backend(const std::string& backend) {
    return backend == "rocm" || backend == "rocm-stable";
}

std::string resolve_mlx_backend(const std::string& backend) {
    if (!backend.empty() && backend != "auto") {
        return backend;
    }

#if defined(__APPLE__)
    return "metal";
#else
    if (is_supported_rocm_arch(SystemInfo::get_rocm_arch())) {
        return "rocm";
    }
    throw std::runtime_error(
        "No supported lemon-mlx backend is available on this system. "
        "Use Apple Silicon Metal or Linux ROCm on gfx1151. "
        "The CPU implementation is retained for development but is not "
        "currently exposed because its performance is not competitive.");
#endif
}

void append_path(std::vector<fs::path>& paths, const fs::path& path) {
    if (path.empty()) {
        return;
    }

    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return;
    }

    const std::string normalized = path.lexically_normal().string();
    for (const auto& existing : paths) {
        if (existing.lexically_normal().string() == normalized) {
            return;
        }
    }
    paths.push_back(path);
}

std::string join_paths(const std::vector<fs::path>& paths, const char* existing_env) {
    std::string joined;
    for (const auto& path : paths) {
        if (!joined.empty()) {
            joined += ":";
        }
        joined += path.string();
    }

    if (existing_env && *existing_env) {
        if (!joined.empty()) {
            joined += ":";
        }
        joined += existing_env;
    }

    return joined;
}

fs::path resolve_mlx_rocm_root() {
    // BackendManager installs the pinned TheRock runtime when the available
    // system ROCm is absent or incompatible. Prefer that exact runtime when it
    // is present so the MLX process cannot fall back to a mismatched /opt/rocm.
    const std::string arch = SystemInfo::get_rocm_arch();
    const std::string therock_lib =
        arch.empty() ? "" : BackendUtils::get_therock_lib_path(arch);
    if (!therock_lib.empty()) {
        return fs::path(therock_lib).parent_path().lexically_normal();
    }

    // If TheRock was unnecessary, use Lemonade's shared system resolution:
    // ROCM_PATH, rocm-sdk, then the platform default such as /opt/rocm.
    const auto system_root = BackendUtils::resolve_rocm_root();
    if (system_root) {
        return system_root->lexically_normal();
    }

    throw std::runtime_error(
        "No compatible ROCm runtime is available for lemon-mlx");
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::vector<std::string> tokenize_quoted_args(const std::string& input) {
    std::vector<std::string> tokens;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    bool token_started = false;

    for (char ch : input) {
        if (escaped) {
            current.push_back(ch);
            escaped = false;
            token_started = true;
            continue;
        }

        if (ch == '\\' && !in_single_quote) {
            escaped = true;
            token_started = true;
            continue;
        }

        if (ch == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
            token_started = true;
            continue;
        }

        if (ch == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
            token_started = true;
            continue;
        }

        if (std::isspace(static_cast<unsigned char>(ch)) && !in_single_quote && !in_double_quote) {
            if (token_started) {
                tokens.push_back(current);
                current.clear();
                token_started = false;
            }
            continue;
        }

        current.push_back(ch);
        token_started = true;
    }

    if (escaped) {
        throw std::runtime_error("Invalid lemon-mlx args: trailing escape");
    }
    if (in_single_quote || in_double_quote) {
        throw std::runtime_error("Invalid lemon-mlx args: unmatched quote");
    }
    if (token_started) {
        tokens.push_back(current);
    }

    return tokens;
}

struct ReasoningParts {
    std::string content;
    std::string reasoning;
};

struct MlxTelemetrySnapshot {
    int input_tokens = 0;
    int output_tokens = 0;
    double time_to_first_token = 0.0;
    double tokens_per_second = 0.0;
};

constexpr const char* kThinkStart = "<think>";
constexpr const char* kThinkEnd = "</think>";

const std::vector<std::string>& mlx_stop_sequences() {
    static const std::vector<std::string> stops = {
        "<|endoftext|>",
        "<|im_start|>",
        "<|im_end|>",
        "<start_of_turn>",
        "<end_of_turn>",
    };
    return stops;
}

std::vector<std::string> request_stop_sequences(const json& request) {
    std::vector<std::string> stops;
    if (!request.contains("stop") || request["stop"].is_null()) {
        return stops;
    }

    const auto& value = request["stop"];
    if (value.is_string()) {
        const std::string stop = value.get<std::string>();
        if (!stop.empty()) {
            stops.push_back(stop);
        }
        return stops;
    }

    if (value.is_array()) {
        for (const auto& item : value) {
            if (!item.is_string()) {
                continue;
            }
            const std::string stop = item.get<std::string>();
            if (!stop.empty()) {
                stops.push_back(stop);
            }
        }
    }

    return stops;
}

double seconds_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

bool json_number(const json& value) {
    return value.is_number_integer() || value.is_number_unsigned() || value.is_number_float();
}

int read_int(const json& object, const char* key) {
    return object.contains(key) && json_number(object[key]) ? object[key].get<int>() : 0;
}

double read_double(const json& object, const char* key) {
    return object.contains(key) && json_number(object[key]) ? object[key].get<double>() : 0.0;
}

void consider_stop_sequences(const std::string& text,
                             const std::vector<std::string>& stops,
                             size_t& first) {
    for (const auto& stop : stops) {
        if (stop.empty()) {
            continue;
        }
        const size_t pos = text.find(stop);
        if (pos != std::string::npos && (first == std::string::npos || pos < first)) {
            first = pos;
        }
    }
}

void truncate_at_stop_sequence(
    std::string& text,
    const std::vector<std::string>& request_stops = {}) {
    size_t first = std::string::npos;
    consider_stop_sequences(text, mlx_stop_sequences(), first);
    consider_stop_sequences(text, request_stops, first);
    if (first != std::string::npos) {
        text.erase(first);
    }
}

ReasoningParts split_thinking_tags(const std::string& text) {
    ReasoningParts parts;
    bool in_reasoning = false;
    size_t pos = 0;

    while (pos < text.size()) {
        const size_t start = text.find(kThinkStart, pos);
        const size_t end = text.find(kThinkEnd, pos);

        if (!in_reasoning) {
            if (end != std::string::npos && (start == std::string::npos || end < start)) {
                // Some MLX/Qwen streams have been observed to omit the opening
                // tag in the first delta. Treat the leading text as reasoning
                // rather than leaking it into assistant content.
                parts.reasoning += text.substr(pos, end - pos);
                pos = end + std::strlen(kThinkEnd);
                continue;
            }
            if (start == std::string::npos) {
                parts.content += text.substr(pos);
                break;
            }
            parts.content += text.substr(pos, start - pos);
            pos = start + std::strlen(kThinkStart);
            in_reasoning = true;
        } else {
            if (end == std::string::npos) {
                parts.reasoning += text.substr(pos);
                break;
            }
            parts.reasoning += text.substr(pos, end - pos);
            pos = end + std::strlen(kThinkEnd);
            in_reasoning = false;
        }
    }

    truncate_at_stop_sequence(parts.content);
    truncate_at_stop_sequence(parts.reasoning);
    return parts;
}

void append_string_field(json& object, const char* key, const std::string& value) {
    if (value.empty()) {
        return;
    }
    if (object.contains(key) && object[key].is_string()) {
        object[key] = object[key].get<std::string>() + value;
    } else {
        object[key] = value;
    }
}

void normalize_reasoning_response(
    json& response,
    const std::vector<std::string>& request_stops = {}) {
    if (!response.contains("choices") || !response["choices"].is_array()) {
        return;
    }

    for (auto& choice : response["choices"]) {
        if (!choice.is_object()) {
            continue;
        }

        if (choice.contains("message") && choice["message"].is_object()) {
            auto& message = choice["message"];
            if (message.contains("content") && message["content"].is_string()) {
                std::string text = message["content"].get<std::string>();
                truncate_at_stop_sequence(text, request_stops);
                const ReasoningParts parts = split_thinking_tags(text);
                message["content"] = parts.content;
                append_string_field(message, "reasoning_content", parts.reasoning);
            }
        }

        if (choice.contains("text") && choice["text"].is_string()) {
            std::string text = choice["text"].get<std::string>();
            truncate_at_stop_sequence(text, request_stops);
            choice["text"] = text;
        }
    }
}

void merge_usage_telemetry(const json& usage, MlxTelemetrySnapshot& telemetry) {
    if (!usage.is_object()) {
        return;
    }

    const int prompt_tokens = read_int(usage, "prompt_tokens");
    const int completion_tokens = read_int(usage, "completion_tokens");
    const int total_tokens = read_int(usage, "total_tokens");

    if (prompt_tokens > 0) {
        telemetry.input_tokens = prompt_tokens;
    }
    if (completion_tokens > 0) {
        telemetry.output_tokens = completion_tokens;
    } else if (total_tokens > telemetry.input_tokens) {
        telemetry.output_tokens = total_tokens - telemetry.input_tokens;
    }

    if (telemetry.input_tokens == 0) {
        telemetry.input_tokens = read_int(usage, "input_tokens");
    }
    if (telemetry.output_tokens == 0) {
        telemetry.output_tokens = read_int(usage, "output_tokens");
    }

    const double ttft = read_double(usage, "prefill_duration_ttft");
    telemetry.time_to_first_token = ttft > 0.0 ? ttft : read_double(usage, "time_to_first_token");

    const double tps = read_double(usage, "decoding_speed_tps");
    telemetry.tokens_per_second = tps > 0.0 ? tps : read_double(usage, "tokens_per_second");
}

void merge_timings_telemetry(const json& timings, MlxTelemetrySnapshot& telemetry) {
    if (!timings.is_object()) {
        return;
    }

    const int prompt_n = read_int(timings, "prompt_n");
    const int predicted_n = read_int(timings, "predicted_n");
    if (prompt_n > 0) {
        telemetry.input_tokens = prompt_n;
    }
    if (predicted_n > 0) {
        telemetry.output_tokens = predicted_n;
    }

    const double prompt_ms = read_double(timings, "prompt_ms");
    if (prompt_ms > 0.0) {
        telemetry.time_to_first_token = prompt_ms / 1000.0;
    }

    const double predicted_per_second = read_double(timings, "predicted_per_second");
    if (predicted_per_second > 0.0) {
        telemetry.tokens_per_second = predicted_per_second;
    }
}

void merge_response_telemetry(const json& response, MlxTelemetrySnapshot& telemetry) {
    if (response.contains("usage")) {
        merge_usage_telemetry(response["usage"], telemetry);
    }
    if (response.contains("timings")) {
        merge_timings_telemetry(response["timings"], telemetry);
    }
}

void finalize_telemetry(MlxTelemetrySnapshot& telemetry,
                        double elapsed_seconds,
                        double decode_seconds = 0.0) {
    if (telemetry.time_to_first_token <= 0.0 && elapsed_seconds > 0.0) {
        telemetry.time_to_first_token = elapsed_seconds;
    }

    if (telemetry.tokens_per_second <= 0.0 && telemetry.output_tokens > 0) {
        double seconds = decode_seconds;
        if (seconds <= 0.0 && elapsed_seconds > 0.0) {
            seconds = elapsed_seconds;
            if (telemetry.time_to_first_token > 0.0 && telemetry.time_to_first_token < elapsed_seconds) {
                seconds = elapsed_seconds - telemetry.time_to_first_token;
            }
        }
        if (seconds > 0.0) {
            telemetry.tokens_per_second = telemetry.output_tokens / seconds;
        }
    }
}

void ensure_usage_timing_fields(json& response, const MlxTelemetrySnapshot& telemetry) {
    if (!response.contains("usage") || !response["usage"].is_object()) {
        if (telemetry.input_tokens <= 0 && telemetry.output_tokens <= 0) {
            return;
        }
        response["usage"] = json::object();
    }

    auto& usage = response["usage"];
    if (!usage.contains("prompt_tokens") && telemetry.input_tokens > 0) {
        usage["prompt_tokens"] = telemetry.input_tokens;
    }
    if (!usage.contains("completion_tokens") && telemetry.output_tokens > 0) {
        usage["completion_tokens"] = telemetry.output_tokens;
    }
    if (!usage.contains("prefill_duration_ttft") && telemetry.time_to_first_token > 0.0) {
        usage["prefill_duration_ttft"] = telemetry.time_to_first_token;
    }
    if (!usage.contains("decoding_speed_tps") && telemetry.tokens_per_second > 0.0) {
        usage["decoding_speed_tps"] = telemetry.tokens_per_second;
    }
}


void add_token_estimate_bytes(const json& value, size_t& bytes) {
    if (value.is_string()) {
        bytes += value.get<std::string>().size();
    } else if (value.is_array()) {
        for (const auto& item : value) {
            add_token_estimate_bytes(item, bytes);
        }
    } else if (value.is_object()) {
        for (const auto& item : value.items()) {
            add_token_estimate_bytes(item.value(), bytes);
        }
    }
}

int estimate_prompt_tokens(const json& request) {
    size_t bytes = 0;
    if (request.contains("messages")) {
        add_token_estimate_bytes(request["messages"], bytes);
    } else if (request.contains("prompt")) {
        add_token_estimate_bytes(request["prompt"], bytes);
    }
    return bytes == 0 ? 0 : std::max(1, static_cast<int>((bytes + 3) / 4));
}

void record_mlx_telemetry(json& response, double elapsed_seconds, int prompt_token_fallback = 0) {
    if (response.contains("error")) {
        return;
    }

    MlxTelemetrySnapshot telemetry;
    merge_response_telemetry(response, telemetry);
    if (telemetry.input_tokens <= 0 && prompt_token_fallback > 0) {
        telemetry.input_tokens = prompt_token_fallback;
    }
    finalize_telemetry(telemetry, elapsed_seconds);
    ensure_usage_timing_fields(response, telemetry);
}


class ReasoningStreamNormalizer {
public:
    explicit ReasoningStreamNormalizer(
        bool prefix_reasoning = false,
        std::vector<std::string> request_stops = {})
        : inside_reasoning_(prefix_reasoning),
          request_stops_(std::move(request_stops)) {}

    std::vector<std::pair<std::string, std::string>> consume(const std::string& text) {
        pending_ += text;
        return drain(false);
    }

    std::vector<std::pair<std::string, std::string>> finish() {
        return drain(true);
    }

    bool stopped() const {
        return stopped_;
    }

private:
    std::vector<std::pair<std::string, std::string>> drain(bool flush_all) {
        std::vector<std::pair<std::string, std::string>> out;

        while (!pending_.empty()) {
            if (stopped_) {
                pending_.clear();
                break;
            }

            if (drop_leading_assistant_prefix_) {
                trim_leading_assistant_prefix();
                drop_leading_assistant_prefix_ = false;
                if (pending_.empty()) {
                    break;
                }
            }

            const size_t stop_pos = first_stop_pos(pending_);
            if (stop_pos != std::string::npos) {
                // Some backends occasionally echo a leading chat-control token
                // before the actual answer, especially on the CPU build. Do not
                // treat that as a completed answer before any payload has been
                // emitted; drop the control prefix and keep reading instead.
                if (stop_pos == 0 && !emitted_any_ && drop_leading_stop_marker()) {
                    continue;
                }

                pending_.erase(stop_pos);
                stopped_ = true;
                flush_all = true;
            }

            const size_t start = pending_.find(kThinkStart);
            const size_t end = pending_.find(kThinkEnd);

            if (inside_reasoning_) {
                if (start != std::string::npos && (end == std::string::npos || start < end)) {
                    emit(out, "reasoning_content", pending_.substr(0, start));
                    pending_.erase(0, start + std::strlen(kThinkStart));
                    continue;
                }
                if (end != std::string::npos) {
                    emit(out, "reasoning_content", pending_.substr(0, end));
                    pending_.erase(0, end + std::strlen(kThinkEnd));
                    inside_reasoning_ = false;
                    continue;
                }
            } else {
                if (start != std::string::npos && (end == std::string::npos || start < end)) {
                    emit(out, "content", pending_.substr(0, start));
                    pending_.erase(0, start + std::strlen(kThinkStart));
                    inside_reasoning_ = true;
                    continue;
                }
                if (end != std::string::npos) {
                    // A closing tag without an active reasoning section is not
                    // enough to reclassify bytes that were already streamed as
                    // content. Drop the stray marker instead of creating a late,
                    // misleading Thinking box with only the guarded tail.
                    emit(out, "content", pending_.substr(0, end));
                    pending_.erase(0, end + std::strlen(kThinkEnd));
                    continue;
                }
            }

            const size_t keep = split_guard_bytes();
            if (flush_all || pending_.size() > keep) {
                const size_t emit_len = flush_all ? pending_.size() : pending_.size() - keep;
                emit(out, inside_reasoning_ ? "reasoning_content" : "content",
                     pending_.substr(0, emit_len));
                pending_.erase(0, emit_len);
                continue;
            }

            break;
        }

        return out;
    }

    size_t first_stop_pos(const std::string& text) const {
        size_t first = std::string::npos;
        consider_stop_sequences(text, mlx_stop_sequences(), first);
        consider_stop_sequences(text, request_stops_, first);
        return first;
    }

    size_t split_guard_bytes() const {
        size_t guard = std::max(std::strlen(kThinkStart), std::strlen(kThinkEnd)) - 1;
        for (const auto& stop : mlx_stop_sequences()) {
            if (!stop.empty()) {
                guard = std::max(guard, stop.size() - 1);
            }
        }
        for (const auto& stop : request_stops_) {
            if (!stop.empty()) {
                guard = std::max(guard, stop.size() - 1);
            }
        }
        return guard;
    }

    bool drop_prefix(const std::string& prefix) {
        if (pending_.rfind(prefix, 0) != 0) {
            return false;
        }
        pending_.erase(0, prefix.size());
        return true;
    }

    void trim_leading_assistant_prefix() {
        while (!pending_.empty() &&
               (pending_.front() == ' ' || pending_.front() == '\n' || pending_.front() == '\r' || pending_.front() == '\t')) {
            pending_.erase(pending_.begin());
        }
        static constexpr const char* kAssistant = "assistant";
        if (pending_.rfind(kAssistant, 0) == 0) {
            pending_.erase(0, std::strlen(kAssistant));
            while (!pending_.empty() &&
                   (pending_.front() == ' ' || pending_.front() == '\n' || pending_.front() == '\r' ||
                    pending_.front() == '\t' || pending_.front() == ':')) {
                pending_.erase(pending_.begin());
            }
        }
    }

    bool drop_leading_stop_marker() {
        if (drop_prefix("<|im_start|>") || drop_prefix("<start_of_turn>")) {
            drop_leading_assistant_prefix_ = true;
            trim_leading_assistant_prefix();
            return true;
        }

        return drop_prefix("<|im_end|>") ||
               drop_prefix("<end_of_turn>") ||
               drop_prefix("<|endoftext|>");
    }

    void emit(std::vector<std::pair<std::string, std::string>>& out,
              const std::string& field,
              const std::string& text) {
        if (!text.empty()) {
            emitted_any_ = true;
            out.push_back({field, text});
        }
    }

    std::string pending_;
    bool inside_reasoning_ = false;
    bool stopped_ = false;
    bool emitted_any_ = false;
    bool drop_leading_assistant_prefix_ = false;
    std::vector<std::string> request_stops_;
};

bool request_disables_thinking(const json& request) {
    if (request.contains("enable_thinking") && request["enable_thinking"].is_boolean()) {
        return request["enable_thinking"].get<bool>() == false;
    }

    if (request.contains("thinking")) {
        const auto& thinking = request["thinking"];
        if (thinking.is_boolean()) {
            return thinking.get<bool>() == false;
        }
        if (thinking.is_object() && thinking.value("type", std::string()) == "disabled") {
            return true;
        }
    }

    if (!request.contains("messages") || !request["messages"].is_array()) {
        return false;
    }

    for (const auto& message : request["messages"]) {
        if (!message.is_object() || !message.contains("role") || !message["role"].is_string() ||
            message["role"].get<std::string>() != "user" ||
            !message.contains("content") || !message["content"].is_string()) {
            continue;
        }
        const std::string content = message["content"].get<std::string>();
        if (content.find("/no_think") != std::string::npos) {
            return true;
        }
    }

    return false;
}

std::string request_model_ref(const json& request, const std::string& loaded_model_ref) {
    if (request.contains("model") && request["model"].is_string() && !request["model"].get<std::string>().empty()) {
        return request["model"].get<std::string>();
    }
    return loaded_model_ref;
}

bool is_small_qwen_model(const std::string& model_ref) {
    const std::string model = lowercase_copy(model_ref);
    return model.find("qwen3") != std::string::npos &&
           (model.find("0.8b") != std::string::npos ||
            model.find("0.6b") != std::string::npos);
}

constexpr int kQwen35RocmSafeCtxSize = 4096;

bool is_qwen35_model(const std::string& model_ref) {
    const std::string model = lowercase_copy(model_ref);
    return model.find("qwen3.5") != std::string::npos ||
           model.find("qwen3_5") != std::string::npos;
}

int clamp_qwen35_rocm_ctx_size(int ctx_size,
                               const std::string& model_ref,
                               const std::string& backend) {
    if (!is_mlx_rocm_backend(backend) ||
        ctx_size <= kQwen35RocmSafeCtxSize ||
        !is_qwen35_model(model_ref)) {
        return ctx_size;
    }

    LOG(WARNING, kLog) << "Clamping lemon-mlx ROCm context for Qwen3.5 from "
                       << ctx_size << " to " << kQwen35RocmSafeCtxSize
                       << " to avoid backend startup failure" << std::endl;
    return kQwen35RocmSafeCtxSize;
}

bool looks_like_qwen_model(const std::string& model_ref) {
    return lowercase_copy(model_ref).find("qwen3") != std::string::npos;
}


bool prefers_prefix_reasoning(const json& request,
                              const std::string& loaded_model_ref,
                              bool cpu_backend) {
    if (request_disables_thinking(request)) {
        return false;
    }

    const std::string model_ref = request_model_ref(request, loaded_model_ref);
    if (!looks_like_qwen_model(model_ref)) {
        return false;
    }

    // The prefix-only reasoning workaround is needed for the larger Qwen MLX
    // models that omit the opening <think> in the stream. Keep it away from
    // CPU and tiny Qwen variants: those can otherwise finish with only
    // reasoning_content, which the UI correctly treats as "no answer".
    if (cpu_backend || is_small_qwen_model(model_ref)) {
        return false;
    }

    return true;
}

bool choice_has_role_only(const json& choice) {
    return choice.contains("delta") && choice["delta"].is_object() &&
           choice["delta"].contains("role") && choice["delta"].size() == 1;
}

std::vector<json> transform_chat_stream_chunk(const json& chunk,
                                              ReasoningStreamNormalizer& normalizer) {
    // Reasoning-only transform. Never invent tool_calls from free text.
    // Preserve engine-emitted delta.tool_calls exactly once (do not clone onto
    // every content/reasoning split piece). See tools-plan-lemonade Workstream A.
    if (!chunk.contains("choices") || !chunk["choices"].is_array()) {
        return {chunk};
    }

    std::vector<json> chunks;
    json passthrough = chunk;
    passthrough["choices"] = json::array();

    for (const auto& choice : chunk["choices"]) {
        if (!choice.is_object() || !choice.contains("delta") || !choice["delta"].is_object() ||
            !choice["delta"].contains("content") || !choice["delta"]["content"].is_string()) {
            // Pure tool_calls / finish_reason / role-only / etc. — pass through.
            passthrough["choices"].push_back(choice);
            continue;
        }

        json base_choice = choice;
        // Detach tool_calls before splitting content so they are not duplicated
        // onto every reasoning/content piece.
        json tool_calls_once = json();
        const bool has_tool_calls =
            base_choice["delta"].contains("tool_calls") &&
            !base_choice["delta"]["tool_calls"].is_null() &&
            !(base_choice["delta"]["tool_calls"].is_array() &&
              base_choice["delta"]["tool_calls"].empty());
        if (has_tool_calls) {
            tool_calls_once = base_choice["delta"]["tool_calls"];
            base_choice["delta"].erase("tool_calls");
        }

        const auto pieces = normalizer.consume(choice["delta"]["content"].get<std::string>());
        base_choice["delta"].erase("content");

        if (pieces.empty()) {
            // Content buffered by normalizer; reattach tool_calls once if present.
            if (has_tool_calls) {
                base_choice["delta"]["tool_calls"] = tool_calls_once;
            }
            if (!base_choice["delta"].empty() || choice_has_role_only(base_choice) ||
                (base_choice.contains("finish_reason") && !base_choice["finish_reason"].is_null())) {
                passthrough["choices"].push_back(base_choice);
            }
            continue;
        }

        // Emit content / reasoning_content pieces without tool_calls.
        for (const auto& piece : pieces) {
            json split_chunk = chunk;
            // Usage/timing metadata belongs to the backend's original event.
            // Do not duplicate it into every synthetic reasoning/content piece.
            split_chunk.erase("usage");
            split_chunk.erase("timings");
            json split_choice = base_choice;
            split_choice["delta"][piece.first] = piece.second;
            // Intermediate content pieces should not carry a terminal finish_reason.
            if (split_choice.contains("finish_reason") &&
                !split_choice["finish_reason"].is_null()) {
                split_choice["finish_reason"] = nullptr;
            }
            split_chunk["choices"] = json::array({split_choice});
            chunks.push_back(split_chunk);
        }

        // Emit tool_calls exactly once after content pieces (if any).
        if (has_tool_calls) {
            json tools_choice = json{
                {"index", base_choice.value("index", 0)},
                {"delta", {{"tool_calls", tool_calls_once}}},
                {"finish_reason", nullptr},
            };
            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                tools_choice["finish_reason"] = choice["finish_reason"];
            }
            passthrough["choices"].push_back(std::move(tools_choice));
        } else if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            // Preserve finish_reason that arrived with content-only delta.
            json fr_choice = json{
                {"index", base_choice.value("index", 0)},
                {"delta", json::object()},
                {"finish_reason", choice["finish_reason"]},
            };
            passthrough["choices"].push_back(std::move(fr_choice));
        }
    }

    if (!passthrough["choices"].empty() || (chunk.contains("usage") && !chunk["usage"].is_null())) {
        chunks.insert(chunks.begin(), passthrough);
    }

    return chunks;
}

std::vector<json> flush_chat_stream_chunk(const json& template_chunk,
                                          ReasoningStreamNormalizer& normalizer) {
    std::vector<json> chunks;
    for (const auto& piece : normalizer.finish()) {
        json chunk = template_chunk.is_object() ? template_chunk : json::object();
        chunk.erase("usage");
        chunk.erase("timings");
        chunk["choices"] = json::array({{{"delta", {{piece.first, piece.second}}}, {"index", 0}, {"finish_reason", nullptr}}});
        chunks.push_back(chunk);
    }
    return chunks;
}

void merge_stream_telemetry_from_chunk(const json& chunk, MlxTelemetrySnapshot& telemetry) {
    if (chunk.contains("usage")) {
        merge_usage_telemetry(chunk["usage"], telemetry);
    }
    if (chunk.contains("timings")) {
        merge_timings_telemetry(chunk["timings"], telemetry);
    }
}

int estimate_streamed_tokens(const json& chunk) {
    if (!chunk.contains("choices") || !chunk["choices"].is_array()) {
        return 0;
    }

    size_t bytes = 0;
    for (const auto& choice : chunk["choices"]) {
        if (!choice.is_object()) {
            continue;
        }
        if (choice.contains("delta") && choice["delta"].is_object()) {
            const auto& delta = choice["delta"];
            for (const char* key : {"content", "reasoning_content"}) {
                if (delta.contains(key) && delta[key].is_string()) {
                    bytes += delta[key].get<std::string>().size();
                }
            }
        }
        if (choice.contains("text") && choice["text"].is_string()) {
            bytes += choice["text"].get<std::string>().size();
        }
    }

    return bytes == 0 ? 0 : std::max(1, static_cast<int>((bytes + 3) / 4));
}


std::string trim_copy(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string normalized_repetition_text(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\t') {
            ch = ' ';
        }
    }
    value = lowercase_copy(trim_copy(value));
    while (!value.empty() && (value.back() == '.' || value.back() == '!' || value.back() == '?' ||
                              value.back() == ':' || value.back() == ';' || value.back() == ',')) {
        value.pop_back();
    }
    return trim_copy(value);
}

std::vector<std::string> split_words(const std::string& text) {
    std::vector<std::string> words;
    std::istringstream stream(text);
    std::string word;
    while (stream >> word) {
        word = normalized_repetition_text(word);
        if (!word.empty()) {
            words.push_back(word);
        }
    }
    return words;
}

class SmallQwenRepetitionStopper {
public:
    explicit SmallQwenRepetitionStopper(bool enabled) : enabled_(enabled) {}

    bool should_stop(const json& chunk) {
        if (!enabled_) {
            return false;
        }

        const std::string text = extract_text(chunk);
        if (text.empty()) {
            return false;
        }

        transcript_ += text;
        if (transcript_.size() > 4096) {
            transcript_.erase(0, transcript_.size() - 4096);
        }

        return repeated_line() || repeated_tail_word();
    }

private:
    std::string extract_text(const json& chunk) const {
        if (!chunk.contains("choices") || !chunk["choices"].is_array()) {
            return {};
        }

        std::string text;
        for (const auto& choice : chunk["choices"]) {
            if (!choice.is_object()) {
                continue;
            }
            if (choice.contains("delta") && choice["delta"].is_object()) {
                const auto& delta = choice["delta"];
                for (const char* key : {"content", "reasoning_content"}) {
                    if (delta.contains(key) && delta[key].is_string()) {
                        text += delta[key].get<std::string>();
                    }
                }
            }
            if (choice.contains("text") && choice["text"].is_string()) {
                text += choice["text"].get<std::string>();
            }
        }
        return text;
    }

    bool repeated_line() const {
        std::vector<std::string> lines;
        std::istringstream stream(transcript_);
        std::string line;
        while (std::getline(stream, line)) {
            line = normalized_repetition_text(line);
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        if (lines.size() < 5) {
            return false;
        }

        const std::string& last = lines.back();
        int repeats = 0;
        for (auto it = lines.rbegin(); it != lines.rend() && *it == last; ++it) {
            ++repeats;
        }
        return repeats >= 5;
    }

    bool repeated_tail_word() const {
        const auto words = split_words(transcript_);
        if (words.size() < 10) {
            return false;
        }

        const std::string& last = words.back();
        if (last.size() < 2) {
            return false;
        }

        int repeats = 0;
        for (auto it = words.rbegin(); it != words.rend() && *it == last; ++it) {
            ++repeats;
        }
        return repeats >= 10;
    }

    bool enabled_ = false;
    std::string transcript_;
};

std::vector<json> stream_chunks_from_blocking_response(
    json response,
    bool chat_response,
    const std::vector<std::string>& request_stops = {}) {
    normalize_reasoning_response(response, request_stops);

    std::vector<json> chunks;
    if (!response.contains("choices") || !response["choices"].is_array()) {
        return chunks;
    }

    const std::string id = response.value("id", std::string());
    const int64_t created = response.value("created", int64_t{0});
    const std::string model = response.value("model", std::string());

    for (const auto& choice : response["choices"]) {
        if (!choice.is_object()) {
            continue;
        }
        const int index = choice.value("index", 0);

        if (chat_response && choice.contains("message") && choice["message"].is_object()) {
            const auto& message = choice["message"];
            const std::string reasoning = message.value("reasoning_content", std::string());
            // content may be null when only tool_calls are present.
            std::string content;
            if (message.contains("content") && message["content"].is_string()) {
                content = message["content"].get<std::string>();
            }
            const bool has_tool_calls =
                message.contains("tool_calls") && message["tool_calls"].is_array() &&
                !message["tool_calls"].empty();
            std::string finish_reason = choice.value("finish_reason", std::string("stop"));
            if (has_tool_calls && (finish_reason.empty() || finish_reason == "stop")) {
                // Prefer OpenAI tool_calls finish when structured calls are present.
                finish_reason = "tool_calls";
            }

            json role_chunk = {
                {"id", id}, {"object", "chat.completion.chunk"}, {"created", created}, {"model", model},
                {"choices", json::array({{{"index", index}, {"delta", {{"role", "assistant"}}}, {"finish_reason", nullptr}}})}
            };
            chunks.push_back(std::move(role_chunk));

            if (!reasoning.empty()) {
                json reasoning_chunk = {
                    {"id", id}, {"object", "chat.completion.chunk"}, {"created", created}, {"model", model},
                    {"choices", json::array({{{"index", index}, {"delta", {{"reasoning_content", reasoning}}}, {"finish_reason", nullptr}}})}
                };
                chunks.push_back(std::move(reasoning_chunk));
            }
            if (!content.empty()) {
                json content_chunk = {
                    {"id", id}, {"object", "chat.completion.chunk"}, {"created", created}, {"model", model},
                    {"choices", json::array({{{"index", index}, {"delta", {{"content", content}}}, {"finish_reason", nullptr}}})}
                };
                chunks.push_back(std::move(content_chunk));
            }
            // Preserve structured tool_calls (do not invent from free text).
            if (has_tool_calls) {
                json tools_chunk = {
                    {"id", id},
                    {"object", "chat.completion.chunk"},
                    {"created", created},
                    {"model", model},
                    {"choices",
                     json::array({{{"index", index},
                                   {"delta", {{"tool_calls", message["tool_calls"]}}},
                                   {"finish_reason", nullptr}}})}
                };
                chunks.push_back(std::move(tools_chunk));
            }
            // Terminal finish_reason for this choice (used by send_final_done tracker).
            json finish_chunk = {
                {"id", id},
                {"object", "chat.completion.chunk"},
                {"created", created},
                {"model", model},
                {"choices",
                 json::array({{{"index", index},
                               {"delta", json::object()},
                               {"finish_reason", finish_reason}}})}
            };
            chunks.push_back(std::move(finish_chunk));
        } else if (!chat_response && choice.contains("text") && choice["text"].is_string()) {
            json chunk = {
                {"id", id}, {"object", "text_completion.chunk"}, {"created", created}, {"model", model},
                {"choices", json::array({{{"index", index}, {"text", choice["text"].get<std::string>()}, {"finish_reason", nullptr}}})}
            };
            chunks.push_back(std::move(chunk));
        }
    }

    return chunks;
}

std::string sse_data_event(const json& chunk) {
    return "data: " + chunk.dump() + "\n\n";
}


} // namespace

InstallParams MlxServer::get_install_params(const std::string& backend, const std::string& version) {
    InstallParams params;
    params.repo = "lemonade-sdk/lemon-mlx-engine";

    const std::string resolved = resolve_mlx_backend(backend);
    if (resolved == "system") {
        return params;
    }

    if (resolved == "metal") {
        if (!is_macos_arm64()) {
            throw std::runtime_error("Metal lemon-mlx requires Apple Silicon macOS");
        }
        params.filename = "mlx-engine-" + version + "-macos-arm64.zip";
        return params;
    }

    if (is_mlx_rocm_backend(resolved)) {
        if (!is_linux_x64()) {
            throw std::runtime_error(
                "ROCm lemon-mlx requires Linux x86_64");
        }

        const std::string arch = SystemInfo::get_rocm_arch();
        if (!is_supported_rocm_arch(arch)) {
            throw std::runtime_error(
                SystemInfo::get_unsupported_backend_error(
                    kRecipe, "rocm"));
        }

        params.filename =
            "mlx-engine-" + version +
            "-ubuntu-rocm-x64.zip";

        return params;
    }

    if (resolved == "cpu") {
#if defined(__linux__)
        if (!is_linux_x64()) {
            throw std::runtime_error("CPU lemon-mlx requires Linux x86_64");
        }
        params.filename = "mlx-engine-" + version + "-ubuntu-cpu-x64.zip";
#elif defined(__APPLE__)
        if (!is_macos_arm64()) {
            throw std::runtime_error("CPU lemon-mlx requires Apple Silicon macOS");
        }
        params.filename = "mlx-engine-" + version + "-macos-arm64.zip";
#else
        throw std::runtime_error("CPU lemon-mlx is not supported on this platform");
#endif
        return params;
    }

    throw std::runtime_error("Unknown lemon-mlx backend: " + resolved);
}

MlxServer::MlxServer(const std::string& log_level,
                     ModelManager* model_manager,
                     BackendManager* backend_manager)
    : WrappedServer(kRecipe, log_level, model_manager, backend_manager) {
}

MlxServer::~MlxServer() {
    unload();
}

DeviceType MlxServer::effective_device(const RecipeOptions& options) const {
    const std::string configured_backend = options.get_option("lemon-mlx_backend");
    const std::string backend = resolve_mlx_backend(configured_backend);
    return (backend == "cpu") ? DEVICE_CPU : DEVICE_GPU;
}

void MlxServer::load(const std::string& model_name,
                     const ModelInfo& model_info,
                     const RecipeOptions& options,
                     bool do_not_upgrade) {
    (void)do_not_upgrade;

    LOG(INFO, kLog) << "Loading model: " << model_name << std::endl;
    LOG(DEBUG, kLog) << "Per-model settings: " << options.to_log_string() << std::endl;

    const int ctx_size = options.get_option("ctx_size");
    const std::string configured_backend = options.get_option("lemon-mlx_backend");
    const std::string backend = resolve_mlx_backend(configured_backend);
    const std::string custom_args = options.get_option("lemon-mlx_args");

    RuntimeConfig::validate_backend_choice(kRecipe, configured_backend.empty() ? "auto" : configured_backend);
    LOG(INFO, kLog) << "Using lemon-mlx backend: " << backend << std::endl;

    device_type_ = (backend == "cpu") ? DEVICE_CPU : DEVICE_GPU;
    backend_manager_->install_backend(mlx::spec()->recipe, backend);

    std::string model_ref = model_info.checkpoint();
    if (model_ref.empty()) {
        model_ref = model_info.resolved_path();
    }
    if (model_ref.empty()) {
        throw std::runtime_error("lemon-mlx: no model checkpoint or path provided");
    }
    loaded_model_ref_ = model_ref;

    port_ = choose_port();
    const std::string executable = BackendUtils::get_backend_binary_path(*mlx::spec(), backend);

    const int effective_ctx_size = clamp_qwen35_rocm_ctx_size(ctx_size, model_ref, backend);

    std::vector<std::string> args = {
        model_ref,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
    };

    if (effective_ctx_size > 0) {
        args.push_back("--ctx-size");
        args.push_back(std::to_string(effective_ctx_size));
    }

    if (!custom_args.empty()) {
        const auto parsed_args = tokenize_quoted_args(custom_args);
        args.insert(args.end(), parsed_args.begin(), parsed_args.end());
    }

    std::vector<std::pair<std::string, std::string>> env_vars;
    const std::string hf_cache_dir = get_hf_cache_dir();
    if (!hf_cache_dir.empty()) {
        env_vars.push_back({"HF_HUB_CACHE", hf_cache_dir});
        LOG(DEBUG, kLog) << "Setting HF_HUB_CACHE=" << hf_cache_dir << std::endl;
    }

    const fs::path executable_dir = fs::path(executable).parent_path();

#if defined(__linux__)
    if (is_mlx_rocm_backend(backend)) {
        const fs::path rocm_root = resolve_mlx_rocm_root();

        std::vector<fs::path> lib_paths;
        append_path(lib_paths, rocm_root / "lib");
        append_path(lib_paths, rocm_root / "lib64");
        append_path(lib_paths, rocm_root / "lib" / "llvm" / "lib");
        append_path(lib_paths, executable_dir);

        std::vector<fs::path> bin_paths;
        append_path(bin_paths, rocm_root / "bin");
        append_path(bin_paths, rocm_root / "lib" / "llvm" / "bin");

        env_vars.push_back({
            "LD_LIBRARY_PATH",
            join_paths(lib_paths, std::getenv("LD_LIBRARY_PATH"))
        });
        env_vars.push_back({
            "PATH",
            join_paths(bin_paths, std::getenv("PATH"))
        });
        env_vars.push_back({"ROCM_HOME", rocm_root.string()});
        env_vars.push_back({"ROCM_PATH", rocm_root.string()});
        env_vars.push_back({"HIP_PATH", rocm_root.string()});

        LOG(DEBUG, kLog)
            << "Configured ROCm runtime root for lemon-mlx: "
            << rocm_root << std::endl;
    } else if (backend == "cpu") {
        std::vector<fs::path> lib_paths;
        append_path(lib_paths, executable_dir);
        env_vars.push_back({"LD_LIBRARY_PATH", join_paths(lib_paths, std::getenv("LD_LIBRARY_PATH"))});
        env_vars.push_back({"MLX_DISABLE_COMPILE", "1"});
        LOG(DEBUG, kLog) << "Configured CPU runtime environment for lemon-mlx" << std::endl;
    }
#elif defined(__APPLE__)
    std::vector<fs::path> lib_paths;
    append_path(lib_paths, executable_dir);
    env_vars.push_back({"DYLD_LIBRARY_PATH", join_paths(lib_paths, std::getenv("DYLD_LIBRARY_PATH"))});
#endif

    // Always log executable + argv at INFO so operators can audit custom args
    // (e.g. --no-think) without requiring DEBUG/inherit_output.
    {
        std::ostringstream argv_line;
        argv_line << executable;
        for (const auto& a : args) {
            argv_line << ' ' << a;
        }
        LOG(INFO, kLog) << "Starting lemon-mlx server: " << argv_line.str()
                        << std::endl;
    }
    const bool inherit_output = (log_level_ == "info") || is_debug();
    set_process_handle(ProcessManager::start_process(executable, args, "", inherit_output, true, env_vars));

    if (!wait_for_ready("/health")) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("lemon-mlx server failed to start");
    }

    LOG(DEBUG, kLog) << "Model loaded on port " << port_ << std::endl;
}

void MlxServer::unload() {
    stop_backend_watchdog();
    LOG(INFO, kLog) << "Unloading model..." << std::endl;

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        ProcessManager::stop_process(handle);
    }

    loaded_model_ref_.clear();
}

json MlxServer::prepare_request(const json& request) const {
    json modified = request;
    if (!loaded_model_ref_.empty()) {
        modified["model"] = loaded_model_ref_;
    }

    if (modified.contains("max_completion_tokens") && !modified.contains("max_tokens")) {
        modified["max_tokens"] = modified["max_completion_tokens"];
    }

    if (is_small_qwen_model(request_model_ref(modified, loaded_model_ref_))) {
        // Tiny Qwen variants are much more repetition-prone. Use a narrow
        // backend-supported penalty only when the caller did not choose one.
        const bool has_penalty = modified.contains("repetition_penalty") &&
            (modified["repetition_penalty"].is_number_integer() ||
             modified["repetition_penalty"].is_number_unsigned() ||
             modified["repetition_penalty"].is_number_float()) &&
            modified["repetition_penalty"].get<double>() > 1.0;
        if (!has_penalty) {
            modified["repetition_penalty"] = 1.25;
        }
    }

    if (modified.value("stream", false)) {
        if (!modified.contains("stream_options") || !modified["stream_options"].is_object()) {
            modified["stream_options"] = json::object();
        }
        modified["stream_options"]["include_usage"] = true;
    }
    return modified;
}

json MlxServer::chat_completion(const json& request) {
    const auto started = std::chrono::steady_clock::now();
    json prepared = prepare_request(request);
    json response = forward_request("/v1/chat/completions", prepared);
    normalize_reasoning_response(response, request_stop_sequences(prepared));
    record_mlx_telemetry(
        response,
        seconds_since(started),
        estimate_prompt_tokens(prepared));
    return response;
}

json MlxServer::completion(const json& request) {
    const auto started = std::chrono::steady_clock::now();
    json prepared = prepare_request(request);
    json response = forward_request("/v1/completions", prepared);
    normalize_reasoning_response(response, request_stop_sequences(prepared));
    record_mlx_telemetry(
        response,
        seconds_since(started),
        estimate_prompt_tokens(prepared));
    return response;
}

json MlxServer::responses(const json& request) {
    (void)request;
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", kRecipe)
    );
}

void MlxServer::forward_streaming_request(const std::string& endpoint,
                                          const std::string& request_body,
                                          httplib::DataSink& sink,
                                          bool sse,
                                          long timeout_seconds,
                                          TelemetryCallback telemetry_callback) {
    json request;
    json prepared_request;
    std::string prepared_body = request_body;
    try {
        request = json::parse(request_body);
        // This override is only reached from Lemonade's streaming route. Be
        // defensive for clients/routes that omit stream=true; otherwise the MLX
        // engine may return a blocking JSON body and the UI sees an empty SSE.
        request["stream"] = true;
        prepared_request = prepare_request(request);
        prepared_request["stream"] = true;
        prepared_body = prepared_request.dump();
    } catch (const json::exception&) {
        WrappedServer::forward_streaming_request(endpoint, request_body, sink, sse, timeout_seconds, telemetry_callback);
        return;
    }

    const bool normalize_reasoning = endpoint == "/v1/chat/completions";
    const bool supported_stream = normalize_reasoning || endpoint == "/v1/completions";
    if (!sse || !supported_stream) {
        WrappedServer::forward_streaming_request(endpoint, prepared_body, sink, sse, timeout_seconds, telemetry_callback);
        return;
    }

    BackendRequestScope request_scope(*this, BackendRequestKind::Streaming);

    const std::string url = get_base_url() + endpoint;
    const auto started = std::chrono::steady_clock::now();
    auto first_token_at = started;
    bool saw_first_token = false;
    bool stream_error = false;
    std::string stream_error_message;
    bool backend_response_error = false;
    std::string backend_error_body;
    bool client_aborted = false;
    bool locally_finished = false;
    bool has_done_marker = false;
    bool saw_data_event = false;
    int estimated_output_tokens = 0;
    std::string event_buffer;
    MlxTelemetrySnapshot telemetry;
    const std::string stream_model_ref = request_model_ref(prepared_request, loaded_model_ref_);
    ReasoningStreamNormalizer normalizer(
        prefers_prefix_reasoning(prepared_request, loaded_model_ref_, device_type_ == DEVICE_CPU),
        request_stop_sequences(prepared_request));
    SmallQwenRepetitionStopper repetition_stopper(is_small_qwen_model(stream_model_ref));
    json last_chunk = json::object();
    // Track last non-null finish_reason from engine/transformed chunks so the
    // synthetic trailer does not clobber "tool_calls" with "stop".
    std::string last_non_null_finish_reason;
    bool saw_engine_terminal_finish = false;

    const auto note_finish_reasons = [&](const json& chunk) {
        if (!chunk.contains("choices") || !chunk["choices"].is_array()) {
            return;
        }
        for (const auto& choice : chunk["choices"]) {
            if (!choice.is_object() || !choice.contains("finish_reason") ||
                choice["finish_reason"].is_null()) {
                continue;
            }
            if (choice["finish_reason"].is_string()) {
                const auto fr = choice["finish_reason"].get<std::string>();
                if (!fr.empty()) {
                    last_non_null_finish_reason = fr;
                    saw_engine_terminal_finish = true;
                }
            }
        }
    };

    const auto write_chunk = [this, &sink, &saw_first_token, &first_token_at, &estimated_output_tokens,
                              &client_aborted, &note_finish_reasons](const json& chunk) -> bool {
        if (client_aborted) {
            return false;
        }
        note_finish_reasons(chunk);
        const int streamed_tokens = estimate_streamed_tokens(chunk);
        if (streamed_tokens > 0) {
            estimated_output_tokens += streamed_tokens;
            if (!saw_first_token) {
                saw_first_token = true;
                first_token_at = std::chrono::steady_clock::now();
            }
        }
        const std::string data = sse_data_event(chunk);
        if (!sink.write(data.c_str(), data.size())) {
            client_aborted = true;
            return false;
        }
        note_backend_activity();
        return true;
    };

    const auto flush_reasoning = [&]() -> bool {
        for (const auto& chunk : flush_chat_stream_chunk(last_chunk, normalizer)) {
            if (!write_chunk(chunk)) {
                return false;
            }
        }
        return true;
    };

    const auto send_final_done = [&]() -> bool {
        if (has_done_marker) {
            return true;
        }
        if (normalize_reasoning && !flush_reasoning()) {
            return false;
        }

        // Prefer engine-provided finish_reason (e.g. "tool_calls") over a hardcoded
        // "stop". Skip synthesizing a terminal choice when the engine already sent one.
        const std::string finish =
            !last_non_null_finish_reason.empty() ? last_non_null_finish_reason : "stop";
        if (!saw_engine_terminal_finish) {
            json final_chunk = last_chunk.is_object() ? last_chunk : json::object();
            final_chunk.erase("usage");
            final_chunk.erase("timings");
            final_chunk["choices"] = json::array(
                {{{"delta", json::object()}, {"index", 0}, {"finish_reason", finish}}});
            if (!write_chunk(final_chunk)) {
                return false;
            }
        }

        const char* done_marker = "data: [DONE]\n\n";
        if (!sink.write(done_marker, std::strlen(done_marker))) {
            client_aborted = true;
            return false;
        }
        has_done_marker = true;
        return true;
    };

    const auto emit_blocking_response_fallback = [&](const std::string& body) -> bool {
        try {
            json response = json::parse(body);
            merge_response_telemetry(response, telemetry);

            const auto chunks = stream_chunks_from_blocking_response(
                response,
                normalize_reasoning,
                request_stop_sequences(prepared_request));
            if (chunks.empty()) {
                return false;
            }

            for (const auto& chunk : chunks) {
                last_chunk = chunk;
                if (!write_chunk(chunk)) {
                    return false;
                }
                if (repetition_stopper.should_stop(chunk)) {
                    locally_finished = true;
                    break;
                }
            }
            return send_final_done();
        } catch (const std::exception&) {
            return false;
        }
    };

    const auto process_event = [&](const std::string& event) -> bool {
        std::istringstream lines(event);
        std::string line;
        bool had_data = false;

        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.rfind("data:", 0) != 0) {
                continue;
            }

            std::string payload = line.substr(5);
            if (!payload.empty() && payload.front() == ' ') {
                payload.erase(payload.begin());
            }
            had_data = true;
            saw_data_event = true;

            if (payload == "[DONE]") {
                return send_final_done();
            }

            try {
                json chunk = json::parse(payload);
                last_chunk = chunk;
                merge_stream_telemetry_from_chunk(chunk, telemetry);
                const auto transformed_chunks = normalize_reasoning
                    ? transform_chat_stream_chunk(chunk, normalizer)
                    : std::vector<json>{chunk};
                for (const auto& transformed : transformed_chunks) {
                    if (!write_chunk(transformed)) {
                        return false;
                    }
                    if (repetition_stopper.should_stop(transformed)) {
                        locally_finished = true;
                        send_final_done();
                        return false;
                    }
                }
                if (normalize_reasoning && normalizer.stopped()) {
                    locally_finished = true;
                    send_final_done();
                    return false;
                }
            } catch (const std::exception&) {
                const std::string passthrough = "data: " + payload + "\n\n";
                if (!sink.write(passthrough.c_str(), passthrough.size())) {
                    client_aborted = true;
                    return false;
                }
                note_backend_activity();
            }
        }

        if (!had_data && !event.empty()) {
            const std::string passthrough = event + "\n\n";
            if (!sink.write(passthrough.c_str(), passthrough.size())) {
                client_aborted = true;
                return false;
            }
            note_backend_activity();
            return true;
        }
        return true;
    };

    try {
        auto result = utils::HttpClient::post_stream(
            url,
            prepared_body,
            [&](const char* data, size_t length) {
                if (backend_response_error) {
                    backend_error_body.append(data, length);
                    return true;
                }
                event_buffer.append(data, length);

                while (true) {
                    const size_t lf = event_buffer.find("\n\n");
                    const size_t crlf = event_buffer.find("\r\n\r\n");
                    if (lf == std::string::npos && crlf == std::string::npos) {
                        break;
                    }
                    const bool use_crlf = crlf != std::string::npos &&
                        (lf == std::string::npos || crlf < lf);
                    const size_t pos = use_crlf ? crlf : lf;
                    const size_t delimiter = use_crlf ? 4 : 2;
                    const std::string event = event_buffer.substr(0, pos);
                    event_buffer.erase(0, pos + delimiter);
                    if (!process_event(event)) {
                        return false;
                    }
                }

                return true;
            },
            {},
            timeout_seconds,
            [&](int status_code) {
                backend_response_error = status_code != 200;
            },
            utils::HttpSecurityPolicy::TrustedLoopback
        );

        if (!locally_finished && !client_aborted &&
            result.status_code != 0 && result.status_code != 200) {
            stream_error = true;
            stream_error_message = "lemon-mlx backend returned HTTP " +
                std::to_string(result.status_code);
            try {
                const json backend_error = json::parse(backend_error_body);
                if (backend_error.contains("error")) {
                    const auto& error = backend_error["error"];
                    if (error.is_string()) {
                        stream_error_message = error.get<std::string>();
                    } else if (error.is_object() && error.contains("message") &&
                               error["message"].is_string()) {
                        stream_error_message = error["message"].get<std::string>();
                    }
                } else if (backend_error.contains("message") &&
                           backend_error["message"].is_string()) {
                    stream_error_message = backend_error["message"].get<std::string>();
                }
            } catch (const json::exception&) {
                if (!backend_error_body.empty()) {
                    constexpr size_t kMaxBackendErrorLength = 4096;
                    const std::string bounded_body =
                        backend_error_body.substr(0, kMaxBackendErrorLength);
                    stream_error_message += ": " + bounded_body;
                    if (backend_error_body.size() > bounded_body.size()) {
                        stream_error_message += "...";
                    }
                }
            }
            LOG(ERROR, kLog) << stream_error_message << std::endl;
        } else if (!locally_finished && !client_aborted &&
                   result.curl_code != 0 && !has_done_marker) {
            stream_error = true;
            stream_error_message = result.curl_error.empty()
                ? std::string("lemon-mlx streaming connection ended unexpectedly")
                : std::string("lemon-mlx streaming failed: ") + result.curl_error;
            LOG(ERROR, kLog) << stream_error_message << std::endl;
        } else if (!locally_finished && !client_aborted &&
                   result.status_code == 0 && !has_done_marker) {
            stream_error = true;
            stream_error_message = "lemon-mlx backend returned no HTTP response";
            LOG(ERROR, kLog) << stream_error_message << std::endl;
        }
    } catch (const std::exception& e) {
        if (client_aborted) {
            LOG(INFO, kLog) << "Streaming request cancelled by client" << std::endl;
        } else if (locally_finished) {
            LOG(DEBUG, kLog) << "Streaming request stopped after local stop sequence" << std::endl;
        } else {
            stream_error = true;
            stream_error_message = e.what();
            LOG(ERROR, kLog) << "Streaming request failed: "
                             << stream_error_message << std::endl;
        }
    }

    if (client_aborted) {
        sink.done();
        return;
    }

    if (!stream_error) {
        if (!event_buffer.empty()) {
            if (!saw_data_event && emit_blocking_response_fallback(event_buffer)) {
                event_buffer.clear();
            } else {
                process_event(event_buffer);
            }
        }
        if (!has_done_marker) {
            send_final_done();
        }

        const double elapsed = seconds_since(started);
        const double decode_seconds = saw_first_token
            ? std::chrono::duration<double>(std::chrono::steady_clock::now() - first_token_at).count()
            : elapsed;
        if (telemetry.input_tokens <= 0) {
            telemetry.input_tokens = estimate_prompt_tokens(prepared_request);
        }
        if (telemetry.output_tokens <= 0 && estimated_output_tokens > 0) {
            telemetry.output_tokens = estimated_output_tokens;
        }
        if (saw_first_token && telemetry.time_to_first_token <= 0.0) {
            telemetry.time_to_first_token = std::chrono::duration<double>(first_token_at - started).count();
        }
        finalize_telemetry(telemetry, elapsed, decode_seconds);
        if (telemetry_callback) {
            telemetry_callback(telemetry.input_tokens,
                               telemetry.output_tokens,
                               telemetry.time_to_first_token,
                               telemetry.tokens_per_second,
                               "");
        }

        LOG(INFO, "Telemetry") << "=== Telemetry ===" << std::endl;
        LOG(INFO, "Telemetry") << "Input tokens:  " << telemetry.input_tokens << std::endl;
        LOG(INFO, "Telemetry") << "Output tokens: " << telemetry.output_tokens << std::endl;
        LOG(INFO, "Telemetry") << "TTFT (s):      " << std::fixed << std::setprecision(3)
                                << telemetry.time_to_first_token << std::endl;
        LOG(INFO, "Telemetry") << "TPS:           " << std::fixed << std::setprecision(2)
                                << telemetry.tokens_per_second << std::endl;
        LOG(INFO, "Telemetry") << "=================" << std::endl;

        sink.done();
        LOG(INFO, "Server") << "Streaming completed - 200 OK" << std::endl;

    } else {
        if (!client_aborted) {
            const json error = {
                {"error", {
                    {"message", stream_error_message.empty()
                        ? std::string("lemon-mlx streaming failed")
                        : stream_error_message},
                    {"type", "backend_error"}
                }}
            };
            const std::string event = sse_data_event(error);
            sink.write(event.c_str(), event.size());
        }
        sink.done();
    }
}

} // namespace backends
} // namespace lemon

namespace lemon {
namespace backends {
namespace mlx {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<MlxServer>(ctx);
}

const BackendSpec* spec() {
    return make_spec<MlxServer>(descriptor);
}

const BackendOps* ops() {
    return default_backend_ops();
}

}  // namespace mlx
}  // namespace backends
}  // namespace lemon
