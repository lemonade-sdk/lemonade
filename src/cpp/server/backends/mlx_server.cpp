#include "lemon/backends/mlx_server.h"
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
constexpr const char* kRocmRuntimeSoname = "libamdhip64.so.7";

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
    // Current lemon-mlx-engine ROCm releases are built on the gfx1151 runner and
    // published as a single ubuntu-rocm-x64 asset. Keep this intentionally
    // narrow until the engine publishes per-arch or verified multi-arch assets.
    return arch == "gfx1151";
}

std::string resolve_mlx_backend(const std::string& backend) {
    if (!backend.empty() && backend != "auto") {
        return backend;
    }

#if defined(__APPLE__)
    return "metal";
#else
    return is_supported_rocm_arch(SystemInfo::get_rocm_arch()) ? "rocm" : "cpu";
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

std::string env_value(const char* name) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : std::string();
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_file(const fs::path& dir, const std::string& filename) {
    std::error_code ec;
    return fs::exists(dir / filename, ec) && !ec;
}

bool contains_rocm_runtime(const fs::path& lib_dir) {
    return contains_file(lib_dir, kRocmRuntimeSoname);
}

bool looks_like_rocm_root(const fs::path& root) {
    return contains_rocm_runtime(root / "lib") || contains_rocm_runtime(root / "lib64");
}

fs::path rocm_root_from_lib_dir(const fs::path& lib_dir) {
    const auto filename = lib_dir.filename().string();
    if (filename == "lib" || filename == "lib64") {
        return lib_dir.parent_path();
    }
    return lib_dir;
}

std::vector<fs::path> rocm_library_paths(const fs::path& executable_dir) {
    std::vector<fs::path> paths;
    append_path(paths, executable_dir);

    const auto append_root = [&paths](const fs::path& root) {
        append_path(paths, root / "lib");
        append_path(paths, root / "lib64");
    };

    const std::string rocm_home = env_value("ROCM_HOME");
    const std::string rocm_path = env_value("ROCM_PATH");
    if (!rocm_home.empty()) {
        append_root(rocm_home);
    }
    if (!rocm_path.empty() && rocm_path != rocm_home) {
        append_root(rocm_path);
    }

    const std::string arch = SystemInfo::get_rocm_arch();
    if (!arch.empty()) {
        try {
            append_path(paths, BackendUtils::get_therock_lib_path(arch));
        } catch (const std::exception& e) {
            LOG(DEBUG, kLog) << "TheRock runtime lookup skipped: " << e.what() << std::endl;
        }
    }

    append_path(paths, "/opt/rocm/core-7.13/lib");
    append_path(paths, "/opt/rocm/core-7.13/lib64");
    append_path(paths, "/opt/rocm/lib");
    append_path(paths, "/opt/rocm/lib64");
    append_path(paths, "/usr/lib/x86_64-linux-gnu");
    append_path(paths, "/usr/local/lib");

    return paths;
}

fs::path resolve_rocm_root(const std::vector<fs::path>& lib_paths) {
    for (const char* var : {"ROCM_HOME", "ROCM_PATH"}) {
        const std::string value = env_value(var);
        if (!value.empty() && looks_like_rocm_root(value)) {
            return fs::path(value).lexically_normal();
        }
    }

    for (const auto& path : lib_paths) {
        if (contains_rocm_runtime(path)) {
            const fs::path root = rocm_root_from_lib_dir(path).lexically_normal();
            if (looks_like_rocm_root(root)) {
                return root;
            }
        }
    }

    for (const fs::path root : {fs::path("/opt/rocm/core-7.13"), fs::path("/opt/rocm")}) {
        if (looks_like_rocm_root(root)) {
            return root;
        }
    }

    return {};
}

void ensure_rocm_runtime_visible(const std::vector<fs::path>& paths) {
    for (const auto& path : paths) {
        if (contains_rocm_runtime(path)) {
            return;
        }
    }

    const std::string existing = env_value("LD_LIBRARY_PATH");
    size_t start = 0;
    while (start <= existing.size()) {
        size_t end = existing.find(':', start);
        std::string item = existing.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!item.empty() && contains_rocm_runtime(item)) {
            return;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }

    throw std::runtime_error(
        "lemon-mlx ROCm runtime not found: missing " + std::string(kRocmRuntimeSoname) +
        ". Reinstall the lemon-mlx ROCm backend so Lemonade installs the TheRock runtime, "
        "or install ROCm 7.13 system-wide and ensure its lib directory is on LD_LIBRARY_PATH.");
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

void truncate_at_stop_sequence(std::string& text) {
    size_t first = std::string::npos;
    for (const auto& stop : mlx_stop_sequences()) {
        const size_t pos = text.find(stop);
        if (pos != std::string::npos && (first == std::string::npos || pos < first)) {
            first = pos;
        }
    }
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

void normalize_reasoning_response(json& response) {
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
                const ReasoningParts parts = split_thinking_tags(message["content"].get<std::string>());
                message["content"] = parts.content;
                append_string_field(message, "reasoning_content", parts.reasoning);
            }
        }

        if (choice.contains("text") && choice["text"].is_string()) {
            std::string text = choice["text"].get<std::string>();
            truncate_at_stop_sequence(text);
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
    explicit ReasoningStreamNormalizer(bool prefix_reasoning = false)
        : inside_reasoning_(prefix_reasoning) {}

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
        for (const auto& stop : mlx_stop_sequences()) {
            const size_t pos = text.find(stop);
            if (pos != std::string::npos && (first == std::string::npos || pos < first)) {
                first = pos;
            }
        }
        return first;
    }

    size_t split_guard_bytes() const {
        size_t guard = std::max(std::strlen(kThinkStart), std::strlen(kThinkEnd)) - 1;
        for (const auto& stop : mlx_stop_sequences()) {
            guard = std::max(guard, stop.size() - 1);
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
    if (!chunk.contains("choices") || !chunk["choices"].is_array()) {
        return {chunk};
    }

    std::vector<json> chunks;
    json passthrough = chunk;
    passthrough["choices"] = json::array();

    for (const auto& choice : chunk["choices"]) {
        if (!choice.is_object() || !choice.contains("delta") || !choice["delta"].is_object() ||
            !choice["delta"].contains("content") || !choice["delta"]["content"].is_string()) {
            passthrough["choices"].push_back(choice);
            continue;
        }

        json base_choice = choice;
        const auto pieces = normalizer.consume(choice["delta"]["content"].get<std::string>());
        base_choice["delta"].erase("content");

        if (pieces.empty()) {
            if (!base_choice["delta"].empty() || choice_has_role_only(base_choice) ||
                (base_choice.contains("finish_reason") && !base_choice["finish_reason"].is_null())) {
                passthrough["choices"].push_back(base_choice);
            }
            continue;
        }

        for (const auto& piece : pieces) {
            json split_chunk = chunk;
            json split_choice = base_choice;
            split_choice["delta"][piece.first] = piece.second;
            split_chunk["choices"] = json::array({split_choice});
            chunks.push_back(split_chunk);
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

std::vector<json> stream_chunks_from_blocking_response(json response, bool chat_response) {
    normalize_reasoning_response(response);

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
            const std::string content = message.value("content", std::string());

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

    if (resolved == "rocm") {
        if (!is_linux_x64()) {
            throw std::runtime_error("ROCm lemon-mlx requires Linux x86_64");
        }
        const std::string arch = SystemInfo::get_rocm_arch();
        if (!is_supported_rocm_arch(arch)) {
            throw std::runtime_error(SystemInfo::get_unsupported_backend_error(kRecipe, "rocm"));
        }
        params.filename = "mlx-engine-" + version + "-ubuntu-rocm-x64.zip";
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
    backend_manager_->install_backend(SPEC.recipe, backend);

    std::string model_ref = model_info.checkpoint();
    if (model_ref.empty()) {
        model_ref = model_info.resolved_path();
    }
    if (model_ref.empty()) {
        throw std::runtime_error("lemon-mlx: no model checkpoint or path provided");
    }
    loaded_model_ref_ = model_ref;

    port_ = choose_port();
    const std::string executable = BackendUtils::get_backend_binary_path(SPEC, backend);

    std::vector<std::string> args = {
        model_ref,
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
    };

    if (ctx_size > 0) {
        args.push_back("--ctx-size");
        args.push_back(std::to_string(ctx_size));
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
    if (backend == "rocm") {
        auto lib_paths = rocm_library_paths(executable_dir);
        ensure_rocm_runtime_visible(lib_paths);

        const fs::path rocm_root = resolve_rocm_root(lib_paths);
        if (rocm_root.empty()) {
            throw std::runtime_error("lemon-mlx ROCm runtime root could not be resolved");
        }

        std::vector<fs::path> bin_paths;
        append_path(bin_paths, rocm_root / "bin");
        append_path(bin_paths, rocm_root / "lib" / "llvm" / "bin");

        env_vars.push_back({"LD_LIBRARY_PATH", join_paths(lib_paths, std::getenv("LD_LIBRARY_PATH"))});
        env_vars.push_back({"PATH", join_paths(bin_paths, std::getenv("PATH"))});
        env_vars.push_back({"ROCM_HOME", rocm_root.string()});
        env_vars.push_back({"ROCM_PATH", rocm_root.string()});
        env_vars.push_back({"HIP_PATH", rocm_root.string()});
        env_vars.push_back({"MLX_ROCM_QMM_DEQUANT_GEMM", "0"});
        LOG(DEBUG, kLog) << "Configured ROCm runtime root for lemon-mlx: " << rocm_root << std::endl;
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

    LOG(INFO, kLog) << "Starting lemon-mlx server..." << std::endl;
    const bool inherit_output = (log_level_ == "info") || is_debug();
    launch_executable_ = executable;
    launch_args_ = args;
    launch_env_vars_ = env_vars;
    launch_inherit_output_ = inherit_output;
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
    launch_executable_.clear();
    launch_args_.clear();
    launch_env_vars_.clear();
    launch_inherit_output_ = false;
}

bool MlxServer::restart_backend_after_cancel() {
    std::lock_guard<std::mutex> lock(backend_restart_mutex_);

    if (launch_executable_.empty() || launch_args_.empty() || port_ == 0) {
        LOG(ERROR, kLog) << "Cannot restart lemon-mlx backend: launch command is not available" << std::endl;
        return false;
    }

    LOG(INFO, kLog) << "Restarting lemon-mlx backend to cancel in-flight generation" << std::endl;
    stop_backend_watchdog();
    if (has_process_handle(process_handle_)) {
        ProcessManager::stop_process(process_handle_);
    }
    process_handle_ = {nullptr, 0};

    try {
        set_process_handle(ProcessManager::start_process(
            launch_executable_, launch_args_, "", launch_inherit_output_, true, launch_env_vars_));
        if (!wait_for_ready("/health", 180)) {
            if (has_process_handle(process_handle_)) {
                ProcessManager::stop_process(process_handle_);
            }
            process_handle_ = {nullptr, 0};
            LOG(ERROR, kLog) << "lemon-mlx backend restart failed" << std::endl;
            return false;
        }
        LOG(INFO, kLog) << "lemon-mlx backend restarted" << std::endl;
        return true;
    } catch (const std::exception& e) {
        process_handle_ = {nullptr, 0};
        LOG(ERROR, kLog) << "lemon-mlx backend restart failed: " << e.what() << std::endl;
        return false;
    }
}

bool MlxServer::ensure_backend_ready() {
    // Always take the restart mutex before trusting process state. During a
    // cancel-triggered restart the new process can exist before /health is
    // ready; requests in that window must wait instead of hitting MLX early and
    // producing an empty stream.
    std::lock_guard<std::mutex> lock(backend_restart_mutex_);
    if (is_process_running() && wait_for_ready("/health", 180)) {
        return true;
    }

    if (launch_executable_.empty() || launch_args_.empty() || port_ == 0) {
        return false;
    }

    LOG(INFO, kLog) << "lemon-mlx backend is not running; restarting before request" << std::endl;
    try {
        stop_backend_watchdog();
        set_process_handle(ProcessManager::start_process(
            launch_executable_, launch_args_, "", launch_inherit_output_, true, launch_env_vars_));
        if (!wait_for_ready("/health", 180)) {
            if (has_process_handle(process_handle_)) {
                ProcessManager::stop_process(process_handle_);
            }
            process_handle_ = {nullptr, 0};
            LOG(ERROR, kLog) << "lemon-mlx backend restart failed" << std::endl;
            return false;
        }
        LOG(INFO, kLog) << "lemon-mlx backend restarted" << std::endl;
        return true;
    } catch (const std::exception& e) {
        process_handle_ = {nullptr, 0};
        LOG(ERROR, kLog) << "lemon-mlx backend restart failed: " << e.what() << std::endl;
        return false;
    }
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
    if (!ensure_backend_ready()) {
        return ErrorResponse::from_exception(
            BackendException(kRecipe, "backend is not ready")
        );
    }

    const auto started = std::chrono::steady_clock::now();
    json prepared = prepare_request(request);
    json response = forward_request("/v1/chat/completions", prepared);
    normalize_reasoning_response(response);
    record_mlx_telemetry(response, seconds_since(started), estimate_prompt_tokens(prepared));
    return response;
}

json MlxServer::completion(const json& request) {
    if (!ensure_backend_ready()) {
        return ErrorResponse::from_exception(
            BackendException(kRecipe, "backend is not ready")
        );
    }

    const auto started = std::chrono::steady_clock::now();
    json prepared = prepare_request(request);
    json response = forward_request("/v1/completions", prepared);
    normalize_reasoning_response(response);
    record_mlx_telemetry(response, seconds_since(started), estimate_prompt_tokens(prepared));
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

    if (!ensure_backend_ready()) {
        const std::string error_msg = "data: {\"error\":{\"message\":\"lemon-mlx backend is not ready: " + server_name_ +
                                      "\",\"type\":\"backend_not_ready\"}}\n\n";
        sink.write(error_msg.c_str(), error_msg.size());
        sink.done();
        return;
    }

    BackendRequestScope request_scope(*this, BackendRequestKind::Streaming);

    const std::string url = get_base_url() + endpoint;
    const auto started = std::chrono::steady_clock::now();
    auto first_token_at = started;
    bool saw_first_token = false;
    bool stream_error = false;
    bool client_aborted = false;
    bool locally_finished = false;
    bool has_done_marker = false;
    bool saw_data_event = false;
    int estimated_output_tokens = 0;
    std::string event_buffer;
    MlxTelemetrySnapshot telemetry;
    const std::string stream_model_ref = request_model_ref(prepared_request, loaded_model_ref_);
    ReasoningStreamNormalizer normalizer(
        prefers_prefix_reasoning(prepared_request, loaded_model_ref_, device_type_ == DEVICE_CPU));
    SmallQwenRepetitionStopper repetition_stopper(is_small_qwen_model(stream_model_ref));
    json last_chunk = json::object();

    const auto write_chunk = [this, &sink, &saw_first_token, &first_token_at, &estimated_output_tokens, &client_aborted](const json& chunk) -> bool {
        if (client_aborted) {
            return false;
        }
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

        json final_chunk = last_chunk.is_object() ? last_chunk : json::object();
        final_chunk.erase("usage");
        final_chunk.erase("timings");
        final_chunk["choices"] = json::array({{{"delta", json::object()}, {"index", 0}, {"finish_reason", "stop"}}});
        if (!write_chunk(final_chunk)) {
            return false;
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

            const auto chunks = stream_chunks_from_blocking_response(response, normalize_reasoning);
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
            timeout_seconds
        );

        if (result.status_code != 200 && !locally_finished && !client_aborted) {
            stream_error = true;
            LOG(ERROR, kLog) << "Backend returned error: " << result.status_code << std::endl;
        }
    } catch (const std::exception& e) {
        if (client_aborted) {
            LOG(INFO, kLog) << "Streaming request cancelled by client" << std::endl;
        } else if (locally_finished) {
            LOG(DEBUG, kLog) << "Streaming request stopped after local stop sequence" << std::endl;
        } else {
            stream_error = true;
            LOG(ERROR, kLog) << "Streaming request failed: " << e.what() << std::endl;
        }
    }

    if (client_aborted) {
        sink.done();
        restart_backend_after_cancel();
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
                               telemetry.tokens_per_second);
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
        if (locally_finished) {
            restart_backend_after_cancel();
        }
    } else {
        sink.done();
    }
}

} // namespace backends
} // namespace lemon
