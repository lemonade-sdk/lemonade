#pragma once

#include <string>
#include <vector>

#include "lemon/utils/custom_args.h"

namespace lemon {
namespace utils {

enum class CustomArgsRequestState {
    Omitted,
    Tombstone,
    Value,
};

struct ScopedCustomArgs {
    std::string backend;
    std::string architecture;
    std::string model_defaults;
    std::string model;
    CustomArgsRequestState request_state = CustomArgsRequestState::Omitted;
    std::string request;
    bool merge_args = true;
};

struct RuntimeArgDefault {
    std::string args;
    std::string flag;
    std::vector<std::string> aliases = {};
};

inline bool is_custom_args_option(const std::string& key) {
    return key != "merge_args" &&
           key.size() > 5 &&
           key.compare(key.size() - 5, 5, "_args") == 0;
}

inline std::string merge_custom_args(const std::string& high,
                                     const std::string& low) {
    if (high.empty()) return low;
    if (low.empty()) return high;

    const auto high_map = build_custom_args_map(parse_custom_args(high, true));
    const auto low_map = build_custom_args_map(parse_custom_args(low, true));
    return map_to_args_string(merge_args_maps(high_map, low_map));
}

inline std::string resolve_scoped_custom_args(const ScopedCustomArgs& args) {
    if (!args.merge_args) {
        return args.request_state == CustomArgsRequestState::Value ? args.request : "";
    }

    if (args.request_state == CustomArgsRequestState::Value) {
        return merge_custom_args(args.request, args.backend);
    }

    const std::string& model_args =
        args.request_state == CustomArgsRequestState::Tombstone
            ? args.model_defaults
            : args.model;
    const std::string model_scope = merge_custom_args(model_args, args.architecture);
    return merge_custom_args(model_scope, args.backend);
}

inline std::string append_runtime_arg_defaults(
    const std::string& custom_args,
    const std::vector<RuntimeArgDefault>& defaults,
    bool enabled = true) {
    if (!enabled) return custom_args;

    std::string result = custom_args;
    std::vector<std::string> resolved_tokens = parse_custom_args(custom_args);

    for (const auto& runtime_default : defaults) {
        bool overridden = custom_args_has_flag(resolved_tokens, runtime_default.flag);
        for (const auto& alias : runtime_default.aliases) {
            overridden = overridden || custom_args_has_flag(resolved_tokens, alias);
        }
        if (overridden || runtime_default.args.empty()) continue;

        if (!result.empty()) result += " ";
        result += runtime_default.args;

        const auto added_tokens = parse_custom_args(runtime_default.args);
        resolved_tokens.insert(resolved_tokens.end(), added_tokens.begin(), added_tokens.end());
    }

    return result;
}

} // namespace utils
} // namespace lemon
