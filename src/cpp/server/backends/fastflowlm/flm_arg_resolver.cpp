#include "lemon/backends/fastflowlm/flm_arg_resolver.h"

#include <charconv>
#include <cctype>
#include <cstddef>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace lemon {
namespace backends {
namespace {

constexpr std::size_t MAX_ARGUMENT_STRING_LENGTH = 4096;
constexpr std::size_t MAX_ARGUMENT_TOKENS = 12;
constexpr long long MAX_SERVE_CONCURRENCY = 1024;

std::vector<std::string> tokenize(const std::string& input) {
    if (input.size() > MAX_ARGUMENT_STRING_LENGTH) {
        throw std::invalid_argument("flm_args exceeds the maximum supported length");
    }

    std::vector<std::string> tokens;
    std::string current;

    for (unsigned char c : input) {
        if (c == '\'' || c == '"' || std::iscntrl(c)) {
            throw std::invalid_argument(
                "flm_args accepts only unquoted, space-separated flag/value pairs");
        }

        if (c == ' ') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(static_cast<char>(c));
        }
    }

    if (!current.empty()) {
        tokens.push_back(current);
    }

    if (tokens.size() > MAX_ARGUMENT_TOKENS) {
        throw std::invalid_argument("flm_args contains too many arguments");
    }

    return tokens;
}

long long parse_integer(const std::string& flag, const std::string& value) {
    long long parsed = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    auto result = std::from_chars(begin, end, parsed);

    if (value.empty() || result.ec != std::errc() || result.ptr != end) {
        throw std::invalid_argument(flag + " requires an integer value");
    }

    return parsed;
}

std::string validate_pmode(const std::string& value) {
    static const std::set<std::string> allowed = {
        "default",
        "powersaver",
        "balanced",
        "performance",
        "turbo",
    };

    if (!allowed.count(value)) {
        throw std::invalid_argument(
            "--pmode must be one of: default, powersaver, balanced, performance, turbo");
    }

    return value;
}

std::string validate_prefill_chunk_len(const std::string& value, int ctx_size) {
    long long parsed = parse_integer("--prefill-chunk-len", value);
    if (parsed != -1 && parsed < 1) {
        throw std::invalid_argument("--prefill-chunk-len must be -1 or a positive integer");
    }
    if (parsed > 0 && ctx_size > 0 && parsed > ctx_size) {
        throw std::invalid_argument("--prefill-chunk-len cannot exceed ctx_size");
    }
    return std::to_string(parsed);
}

std::string validate_img_pre_resize(const std::string& value) {
    long long parsed = parse_integer("--img-pre-resize", value);
    if (parsed < 0 || parsed > 4) {
        throw std::invalid_argument("--img-pre-resize must be between 0 and 4");
    }
    return std::to_string(parsed);
}

std::string validate_concurrency(const std::string& flag, const std::string& value) {
    long long parsed = parse_integer(flag, value);
    if (parsed < 1 || parsed > MAX_SERVE_CONCURRENCY) {
        throw std::invalid_argument(flag + " must be between 1 and 1024");
    }
    return std::to_string(parsed);
}

std::string validate_preemption(const std::string& value) {
    if (value == "1" || value == "true") {
        return "1";
    }
    if (value == "0" || value == "false") {
        return "0";
    }
    throw std::invalid_argument("--preemption must be one of: 0, 1, false, true");
}

std::string validate_value(const std::string& flag, const std::string& value, int ctx_size) {
    if (flag == "--pmode") {
        return validate_pmode(value);
    }
    if (flag == "--prefill-chunk-len") {
        return validate_prefill_chunk_len(value, ctx_size);
    }
    if (flag == "--img-pre-resize") {
        return validate_img_pre_resize(value);
    }
    if (flag == "--socket" || flag == "--q-len") {
        return validate_concurrency(flag, value);
    }
    if (flag == "--preemption") {
        return validate_preemption(value);
    }

    throw std::invalid_argument("flm_args flag is not allowed: " + flag);
}

} // namespace

FLMArgResolution resolve_flm_args(const std::string& flm_args, int ctx_size) {
    std::vector<std::string> tokens = tokenize(flm_args);
    FLMArgResolution resolution;
    std::set<std::string> seen_flags;

    for (std::size_t i = 0; i < tokens.size();) {
        const std::string& flag = tokens[i];
        if (flag.rfind("--", 0) != 0) {
            throw std::invalid_argument(
                "flm_args expects canonical long flags beginning with '--': " + flag);
        }
        if (flag.find('=') != std::string::npos) {
            throw std::invalid_argument(
                "flm_args requires '--flag value' syntax instead of '--flag=value': " + flag);
        }
        if (!seen_flags.insert(flag).second) {
            throw std::invalid_argument("flm_args contains a duplicate flag: " + flag);
        }
        if (i + 1 >= tokens.size()) {
            throw std::invalid_argument("flm_args flag requires a value: " + flag);
        }

        const std::string canonical_value = validate_value(flag, tokens[i + 1], ctx_size);
        resolution.args.push_back(flag);
        resolution.args.push_back(canonical_value);
        i += 2;
    }

    return resolution;
}

} // namespace backends
} // namespace lemon
