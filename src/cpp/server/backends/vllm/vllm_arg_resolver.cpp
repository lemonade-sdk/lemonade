#include "lemon/backends/vllm/vllm_arg_resolver.h"
#include "lemon/utils/custom_args.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

namespace lemon {
namespace backends {
namespace {

struct ParsedArg {
    std::string flag;
    std::vector<std::string> values;
};

constexpr const char* MEMORY_BUDGET_CONFLICT_KEY = "memory_budget";

const std::set<std::string>& protected_flags() {
    // --enforce-eager is deliberately absent: it must reach the resolver, which manages
    // it (see resolve_vllm_args), rather than being rejected here as a process-shape flag.
    static const std::set<std::string> flags = {
        "--enable-prefix-caching",
        "--host",
        "--max-model-len",
        "--model",
        "--port",
        "--served-model-name",
    };
    return flags;
}

std::string conflict_key(const std::string& flag) {
    if (flag == "--tool-call-parser") return "tool_parser";
    if (flag == "--enable-auto-tool-choice" || flag == "--disable-auto-tool-choice") return "tool_choice";
    if (flag == "--quantization") return "quantization";
    if (flag == "--kv-cache-memory-bytes" || flag == "--gpu-memory-utilization") return MEMORY_BUDGET_CONFLICT_KEY;
    return "flag:" + flag;
}

bool is_memory_budget_arg(const ParsedArg& arg) {
    return conflict_key(arg.flag) == MEMORY_BUDGET_CONFLICT_KEY;
}

bool is_generic_conflict_key(const std::string& key) {
    return key.rfind("flag:", 0) == 0;
}

bool is_binary_generic_arg(const ParsedArg& arg) {
    return arg.values.empty() && is_generic_conflict_key(conflict_key(arg.flag));
}

std::set<std::string> repeated_generic_conflict_keys(const std::vector<ParsedArg>& args) {
    std::map<std::string, size_t> counts;
    for (const auto& arg : args) {
        std::string key = conflict_key(arg.flag);
        if (is_generic_conflict_key(key)) {
            ++counts[key];
        }
    }

    std::set<std::string> repeated;
    for (const auto& item : counts) {
        if (item.second > 1) {
            repeated.insert(item.first);
        }
    }
    return repeated;
}

bool is_negative_number_token(const std::string& token) {
    if (token.size() < 2 || token[0] != '-') {
        return false;
    }
    size_t digit_pos = token[1] == '.' ? 2 : 1;
    return digit_pos < token.size() &&
           std::isdigit(static_cast<unsigned char>(token[digit_pos]));
}

bool is_flag_token(const std::string& token) {
    if (token.size() < 2 || token[0] != '-') {
        return false;
    }
    if (is_negative_number_token(token)) {
        return false;
    }
    if (token[1] == '-') {
        return token.size() > 2;
    }
    return true;
}

std::vector<ParsedArg> parse_args(const std::string& arg_string, const std::string& source) {
    std::vector<ParsedArg> parsed;
    std::vector<std::string> tokens = lemon::utils::parse_custom_args(arg_string);

    for (size_t i = 0; i < tokens.size(); ++i) {
        std::string token = tokens[i];
        if (token.empty()) {
            continue;
        }
        if (!is_flag_token(token)) {
            throw std::runtime_error("Invalid vLLM argument '" + token + "' in " + source +
                                     ": expected a flag beginning with '-'");
        }

        ParsedArg arg;
        size_t eq_pos = token.find('=');
        if (eq_pos != std::string::npos) {
            arg.flag = token.substr(0, eq_pos);
            arg.values.push_back(token.substr(eq_pos + 1));
        } else {
            arg.flag = token;
            while (i + 1 < tokens.size() && (tokens[i + 1].empty() || !is_flag_token(tokens[i + 1]))) {
                if (!tokens[i + 1].empty()) {
                    arg.values.push_back(tokens[i + 1]);
                }
                ++i;
            }
        }

        if (protected_flags().count(arg.flag)) {
            throw std::runtime_error("vLLM argument '" + arg.flag + "' in " + source +
                                     " is managed by Lemonade and cannot be overridden");
        }

        parsed.push_back(std::move(arg));
    }

    return parsed;
}

void merge_layer(std::vector<ParsedArg>& target, const std::vector<ParsedArg>& incoming) {
    std::set<std::string> repeatable_keys = repeated_generic_conflict_keys(target);
    std::set<std::string> incoming_repeatable_keys = repeated_generic_conflict_keys(incoming);
    repeatable_keys.insert(incoming_repeatable_keys.begin(), incoming_repeatable_keys.end());

    for (const auto& arg : incoming) {
        std::string key = conflict_key(arg.flag);
        if (!repeatable_keys.count(key)) {
            target.erase(std::remove_if(target.begin(), target.end(),
                                        [&](const ParsedArg& existing) {
                                            return conflict_key(existing.flag) == key;
                                        }),
                         target.end());
        }
        if (is_binary_generic_arg(arg)) {
            std::string negated_flag = lemon::utils::negate_flag(arg.flag);
            if (!negated_flag.empty()) {
                std::string negated_key = conflict_key(negated_flag);
                target.erase(std::remove_if(target.begin(), target.end(),
                                            [&](const ParsedArg& existing) {
                                                return is_binary_generic_arg(existing) &&
                                                       conflict_key(existing.flag) == negated_key;
                                            }),
                             target.end());
            }
        }
        target.push_back(arg);
    }
}

std::vector<std::string> flatten_args(const std::vector<ParsedArg>& parsed) {
    std::vector<std::string> result;
    for (const auto& arg : parsed) {
        result.push_back(arg.flag);
        result.insert(result.end(), arg.values.begin(), arg.values.end());
    }
    return result;
}

const nlohmann::json* find_family(const nlohmann::json& config,
                                  const std::string& family_name) {
    if (!config.contains("families") || !config["families"].is_object()) {
        return nullptr;
    }
    const auto& families = config["families"];
    if (!families.contains(family_name) || !families[family_name].is_object()) {
        throw std::runtime_error("vllm_model_config.json references unknown family '" + family_name + "'");
    }
    return &families[family_name];
}

const nlohmann::json* match_family_by_checkpoint(const nlohmann::json& config,
                                                 const std::string& checkpoint) {
    if (!config.contains("families") || !config["families"].is_object()) {
        return nullptr;
    }

    for (const auto& item : config["families"].items()) {
        const auto& family = item.value();
        if (!family.is_object() || !family.contains("match") || !family["match"].is_array()) {
            continue;
        }

        for (const auto& matcher : family["match"]) {
            if (!matcher.is_object() ||
                !matcher.contains("checkpoint_regex") ||
                !matcher["checkpoint_regex"].is_string()) {
                continue;
            }

            const std::string pattern = matcher["checkpoint_regex"].get<std::string>();
            try {
                if (std::regex_search(checkpoint, std::regex(pattern))) {
                    return &family;
                }
            } catch (const std::regex_error& e) {
                throw std::runtime_error("Invalid checkpoint_regex '" + pattern +
                                         "' in vllm_model_config.json: " + e.what());
            }
        }
    }

    return nullptr;
}

void append_config_args(std::vector<ParsedArg>& target,
                        const nlohmann::json& node,
                        const std::string& source) {
    if (!node.contains("args")) {
        return;
    }
    if (!node["args"].is_string()) {
        throw std::runtime_error(source + " args must be a string");
    }
    merge_layer(target, parse_args(node["args"].get<std::string>(), source));
}

bool has_memory_budget_arg(const std::vector<ParsedArg>& args) {
    return std::any_of(args.begin(), args.end(), is_memory_budget_arg);
}

bool has_dtype_arg(const std::vector<ParsedArg>& args) {
    return std::any_of(args.begin(), args.end(),
                       [](const ParsedArg& arg) { return arg.flag == "--dtype"; });
}

bool has_enforce_eager_arg(const std::vector<ParsedArg>& args) {
    return std::any_of(args.begin(), args.end(),
                       [](const ParsedArg& arg) { return arg.flag == "--enforce-eager"; });
}

const ParsedArg* find_arg(const std::vector<ParsedArg>& args, const std::string& flag) {
    auto it = std::find_if(args.begin(), args.end(),
                           [&](const ParsedArg& arg) { return arg.flag == flag; });
    return it == args.end() ? nullptr : &(*it);
}

} // namespace

VLLMArgResolution resolve_vllm_args(const std::string& model_name,
                                    const std::string& checkpoint,
                                    const nlohmann::json& config,
                                    const std::string& user_vllm_args) {
    if (!config.is_object()) {
        throw std::runtime_error("vllm_model_config.json must contain a JSON object");
    }
    if (config.contains("schema_version") && config["schema_version"] != 1) {
        throw std::runtime_error("Unsupported vllm_model_config.json schema_version");
    }

    std::vector<ParsedArg> resolved;
    const nlohmann::json* model_entry = nullptr;
    if (config.contains("models") && config["models"].is_object() &&
        config["models"].contains(model_name) && config["models"][model_name].is_object()) {
        model_entry = &config["models"][model_name];
    }

    bool enable_checkpoint_regex_match = config.value("enable_checkpoint_regex_match", true);
    bool disable_family_match = model_entry &&
                                model_entry->value("disable_family_match", false);

    const nlohmann::json* family = nullptr;
    if (model_entry && model_entry->contains("family")) {
        if (!(*model_entry)["family"].is_string()) {
            throw std::runtime_error("vllm_model_config.json model '" + model_name +
                                     "' family must be a string");
        }
        family = find_family(config, (*model_entry)["family"].get<std::string>());
    } else if (enable_checkpoint_regex_match && !disable_family_match) {
        family = match_family_by_checkpoint(config, checkpoint);
    }

    if (family) {
        append_config_args(resolved, *family, "vllm_model_config.json family");
    }
    if (model_entry) {
        append_config_args(resolved, *model_entry, "vllm_model_config.json model '" + model_name + "'");
    }

    merge_layer(resolved, parse_args(user_vllm_args, "vllm_args"));

    // Managed, not passthrough: load() re-emits --enforce-eager from the device-class
    // launch policy, so detect it here and strip it to avoid a duplicate on the vLLM
    // command line. Detection lets an explicit request override the graph default.
    const bool has_enforce_eager = has_enforce_eager_arg(resolved);
    resolved.erase(std::remove_if(resolved.begin(), resolved.end(),
                                  [](const ParsedArg& arg) {
                                      return arg.flag == "--enforce-eager";
                                  }),
                   resolved.end());

    const ParsedArg* quantization_arg = find_arg(resolved, "--quantization");
    std::string quantization_value = quantization_arg && !quantization_arg->values.empty()
        ? quantization_arg->values.front()
        : "";

    // A structured JSON knob (e.g. MTP) that cannot ride the args string, so it is read
    // as an object (family first, model wins) and re-serialized for the backend. A
    // scalar/array is rejected loudly rather than dumped as something vLLM cannot parse.
    std::string speculative_config;
    auto take_speculative_config = [&speculative_config](const nlohmann::json* src,
                                                         const char* scope) {
        if (!src || !src->contains("speculative_config")) {
            return;
        }
        const nlohmann::json& sc = (*src)["speculative_config"];
        if (!sc.is_object()) {
            throw std::runtime_error(std::string("speculative_config in the ") + scope +
                                     " entry must be a JSON object, got " + sc.type_name());
        }
        speculative_config = sc.dump();
    };
    take_speculative_config(family, "family");
    take_speculative_config(model_entry, "model");

    return {
        flatten_args(resolved),
        has_memory_budget_arg(resolved),
        has_dtype_arg(resolved),
        has_enforce_eager,
        quantization_arg != nullptr,
        quantization_value,
        speculative_config
    };
}

bool is_discrete_hbm_arch(const std::string& arch) {
    // gfx9* covers every CDNA generation plus Vega20 — all HBM discrete parts, no RDNA
    // (gfx10xx-gfx12xx). Vega20's inclusion is inert: the support gate rejects it first.
    return arch.rfind("gfx9", 0) == 0;
}

DeviceClassLaunchPolicy device_class_launch_policy(const std::string& arch,
                                                   bool has_memory_budget_arg,
                                                   bool has_enforce_eager) {
    const bool discrete_hbm = is_discrete_hbm_arch(arch);
    return {
        /*enforce_eager*/    !discrete_hbm || has_enforce_eager,
        /*force_awq_kernel*/ !discrete_hbm,
        /*cap_kv_cache*/     !discrete_hbm && !has_memory_budget_arg,
    };
}

} // namespace backends
} // namespace lemon
