#include "app_manifest.h"

#include "graph_abi.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace amica_engine {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

bool is_file(const fs::path& path) { return fs::is_regular_file(path); }

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool word_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

json load_json(const fs::path& path) {
    if (!is_file(path)) return json();
    std::string text = read_text_file(path.u8string());
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    return json::parse(text);
}

std::string string_value(const json& object, const char* key) {
    if (!object.is_object()) return "";
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : "";
}

// Like PowerShell's [int64] conversion of a JSON value.
std::int64_t to_int(const json& value, const std::string& what) {
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_number()) return static_cast<std::int64_t>(value.get<double>());
    if (value.is_string()) return std::stoll(value.get<std::string>());
    throw std::runtime_error(what + " is not an integer");
}

std::string first_tensor_type(const EntryPoint& entry, const std::string& path) {
    if (entry.inputs.empty() || entry.inputs[0].rfind("!cgc.tensor<", 0) != 0) {
        throw std::runtime_error("Cannot parse first entry tensor type: " + path);
    }
    return entry.inputs[0];
}

std::string first_result_type(const EntryPoint& entry, const std::string& path) {
    for (const auto& output : entry.outputs) {
        if (output.rfind("!cgc.tensor<", 0) == 0) return output;
    }
    throw std::runtime_error("Cannot parse first entry result type: " + path);
}

bool safetensors_contains(const fs::path& path, const std::string& tensor) {
    if (!is_file(path)) return false;
    try {
        const auto names = safetensors_tensor_names(path.u8string());
        return std::find(names.begin(), names.end(), tensor) != names.end();
    } catch (const std::exception&) {
        return false;
    }
}

bool safetensors_satisfies_graph(const fs::path& resources, const std::string& graph_text) {
    if (!is_file(resources)) return false;
    const auto required = constant_resource_names(graph_text);
    if (required.empty()) return true;
    const auto names = safetensors_tensor_names(resources.u8string());
    const std::set<std::string> available(names.begin(), names.end());
    return std::all_of(required.begin(), required.end(),
                       [&](const std::string& name) { return available.count(name) != 0; });
}

std::string alnum_lower(const std::string& text) {
    std::string result;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::string join(const std::vector<std::string>& parts, const char* separator) {
    std::string result;
    for (std::size_t i = 0; i < parts.size(); ++i) result += (i ? separator : "") + parts[i];
    return result;
}

// Binds a graph's non-activation parameters (arg1 onward) to the tensors in a
// safetensors file whose names end with the same normalized text, or rebuilds
// the binding of a Microsoft LoRA export from its graph; "" when neither gives
// a complete one-to-one map.
std::string entry_resource_map(const std::string& graph_text, const std::vector<std::string>& parameters,
                               const fs::path& resources) {
    const auto tensors = safetensors_tensor_names(resources.u8string());
    std::vector<std::string> result;
    std::set<std::string> matched;
    for (std::size_t index = 1; index < parameters.size(); ++index) {
        const std::string parameter = alnum_lower(parameters[index]);
        std::vector<std::string> matches;
        for (const auto& tensor : tensors) {
            if (ends_with(alnum_lower(tensor), parameter)) matches.push_back(tensor);
        }
        if (matches.size() > 1) {
            // A generic LM-head graph's "norm_weight" matches every layer's norm;
            // the one it needs is the final norm.
            std::vector<std::string> final_matches;
            for (const auto& match : matches) {
                if (lower(match).find("final") != std::string::npos) final_matches.push_back(match);
            }
            if (final_matches.size() == 1) matches = final_matches;
        }
        if (matches.size() == 1) {
            if (!matched.insert(matches[0]).second) {
                result.clear();
                break;
            }
            result.push_back("arg" + std::to_string(index) + "=" + matches[0]);
        }
    }
    if (!parameters.empty() && result.size() == parameters.size() - 1) return join(result, ",");

    // Microsoft LoRA exports name their parameters argN but keep the backbone
    // projections in graph order; each consumes its A then B adapter tensor.
    static const std::string kDequantize = "cgc_op.dequantize_linear(%";
    static const std::string kBackbone = ".matmul.backbone.weight_quantized";
    std::vector<std::string> backbones;
    for (std::size_t at = graph_text.find(kDequantize); at != std::string::npos;
         at = graph_text.find(kDequantize, at + 1)) {
        std::size_t k = at + kDequantize.size();
        const std::size_t begin = k;
        while (k < graph_text.size() &&
               (std::isalnum(static_cast<unsigned char>(graph_text[k])) || std::string("_.$-").find(graph_text[k]) != std::string::npos)) {
            ++k;
        }
        const std::string name = graph_text.substr(begin, k - begin);
        if (!ends_with(name, kBackbone) || (k < graph_text.size() && word_char(graph_text[k]))) continue;
        std::string base = name.substr(0, name.size() - std::string(".backbone.weight_quantized").size());
        base.erase(0, base.find_first_not_of('_'));
        if (std::find(backbones.begin(), backbones.end(), base) == backbones.end()) backbones.push_back(base);
    }
    std::vector<std::string> adapters;
    for (const auto& base : backbones) {
        adapters.push_back(base + ".a_matmul.weight_quantized");
        adapters.push_back(base + ".b_matmul.weight_quantized");
    }
    if (adapters.empty()) return "";
    const std::set<std::string> available(tensors.begin(), tensors.end());
    for (const auto& adapter : adapters) {
        if (!available.count(adapter)) return "";
    }
    if (adapters.size() > parameters.size()) return "";
    result.clear();
    const std::size_t first = parameters.size() - adapters.size();
    for (std::size_t offset = 0; offset < adapters.size(); ++offset) {
        result.push_back("arg" + std::to_string(first + offset) + "=" + adapters[offset]);
    }
    return join(result, ",");
}

std::vector<std::string> token_contents(const json& values) {
    std::vector<std::string> result;
    for (const auto& value : values) {
        if (value.is_string()) result.push_back(value.get<std::string>());
        else if (value.is_object()) result.push_back(string_value(value, "content"));
    }
    return result;
}

// Where an adapted LM-head graph is written: one directory per bundle.
fs::path adapter_directory(const fs::path& bundle) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : lower(fs::weakly_canonical(bundle).u8string())) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    }
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
    return fs::temp_directory_path() / "amica-engine" / name;
}

}  // namespace

std::string adapt_lm_head(const std::string& text, const std::string& producer_type, const std::string& path) {
    const auto fail = [&]() -> std::string { throw std::runtime_error("Cannot adapt LM-head entry point: " + path); };
    // The first entry parameter, "%name : !cgc.tensor<...>".
    const std::size_t keyword = text.find("cgc.entry_point");
    if (keyword == std::string::npos) return fail();
    std::size_t k = text.find('(', text.find('@', keyword));
    if (k == std::string::npos) return fail();
    ++k;
    while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
    if (k >= text.size() || text[k] != '%') return fail();
    const std::size_t name_begin = ++k;
    while (k < text.size() && (std::isalnum(static_cast<unsigned char>(text[k])) || std::string("_.$-").find(text[k]) != std::string::npos)) ++k;
    const std::string input = text.substr(name_begin, k - name_begin);
    while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
    if (k >= text.size() || text[k] != ':') return fail();
    ++k;
    while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
    if (text.compare(k, 12, "!cgc.tensor<") != 0) return fail();
    const std::size_t type_begin = k;
    const std::size_t type_end = text.find('>', k + 12);
    if (input.empty() || type_end == std::string::npos) return fail();
    const std::string consumer_type = text.substr(type_begin, type_end + 1 - type_begin);
    if (consumer_type == producer_type) return text;

    const auto shape = [](const std::string& type) {
        for (const char* dtype : {"!cgc.float16>", "!cgc.float32>"}) {
            if (ends_with(type, dtype)) return type.substr(0, type.size() - std::string(dtype).size()) + ">";
        }
        return type;
    };
    if (shape(consumer_type) != shape(producer_type)) {
        throw std::runtime_error("LM-head input shape '" + consumer_type + "' does not match decoder output '" +
                                 producer_type + "'");
    }
    std::string result = text.substr(0, type_begin) + producer_type + text.substr(type_end + 1);

    // Keep the wider decoder value through an RMSNorm that reads the input
    // directly: its normalized result is bounded and can stay FP16 for the
    // LM-head GEMV, whereas a cast before it can overflow.
    if (ends_with(producer_type, "!cgc.float32>") && ends_with(consumer_type, "!cgc.float16>")) {
        static const std::string kNorm = "cgc_op.rms_normalization";
        for (std::size_t at = result.find(kNorm); at != std::string::npos; at = result.find(kNorm, at + 1)) {
            std::size_t p = at + kNorm.size();
            while (p < result.size() && std::isspace(static_cast<unsigned char>(result[p]))) ++p;
            if (p >= result.size() || result[p] != '(') continue;
            ++p;
            while (p < result.size() && std::isspace(static_cast<unsigned char>(result[p]))) ++p;
            if (result.compare(p, input.size() + 1, "%" + input) != 0) continue;
            p += input.size() + 1;
            if (p < result.size() && word_char(result[p])) continue;
            // The operand types follow the first ":" then "(" after the input.
            for (std::size_t colon = result.find(':', p); colon != std::string::npos; colon = result.find(':', colon + 1)) {
                std::size_t q = colon + 1;
                while (q < result.size() && std::isspace(static_cast<unsigned char>(result[q]))) ++q;
                if (q >= result.size() || result[q] != '(') continue;
                if (result.compare(q + 1, consumer_type.size(), consumer_type) != 0) continue;
                return result.substr(0, q + 1) + producer_type + result.substr(q + 1 + consumer_type.size());
            }
        }
    }

    const std::size_t body = result.find('{', type_end + 1);
    if (body == std::string::npos) throw std::runtime_error("Cannot find LM-head entry body: " + path);
    static const std::string kAdapted = "amica_head_input";
    std::string rest = result.substr(body + 1);
    const std::string reference = "%" + input;
    for (std::size_t at = rest.find(reference); at != std::string::npos; at = rest.find(reference, at)) {
        const std::size_t after = at + reference.size();
        if (after < rest.size() && word_char(rest[after])) {
            at = after;
            continue;
        }
        rest.replace(at, reference.size(), "%" + kAdapted);
        at += kAdapted.size() + 1;
    }
    const std::string cast = "\r\n    %" + kAdapted + " = cgc_op.cast(%" + input + ") : (" + producer_type + ") -> " +
                             consumer_type + "\r\n";
    return result.substr(0, body + 1) + cast + rest;
}

json derive_app_manifest(const std::string& model_dir) {
    const fs::path dir = fs::u8path(model_dir);
    const std::string bundle_name = fs::weakly_canonical(dir).filename().u8string();
    const auto in_dir = [&](const std::string& name) { return dir / fs::u8path(name); };

    // ChatApp qualifies gpt-oss-20b with FP16 activations; BF16-residual graphs
    // are used for the other models that ship both.
    const std::string lower_name = lower(bundle_name);
    const bool gpt_oss_20b = lower_name.rfind("gpt-oss-20b", 0) == 0 &&
                             (lower_name.size() == 11 || !std::isalnum(static_cast<unsigned char>(lower_name[11])));
    const bool bf16_residual = !gpt_oss_20b && is_file(in_dir("decoder_bf16residual.mlir")) &&
                               is_file(in_dir("prefill_bf16residual.mlir"));
    const bool fp16_act = is_file(in_dir("decoder_fp16act.mlir")) && is_file(in_dir("prefill_fp16act.mlir"));
    const bool qdq = !bf16_residual && !fp16_act;
    const std::string decoder_name = bf16_residual ? "decoder_bf16residual.mlir"
                                     : fp16_act    ? "decoder_fp16act.mlir"
                                                   : "decoder.mlir";
    const std::string prefill_name = bf16_residual ? "prefill_bf16residual.mlir"
                                     : fp16_act    ? "prefill_fp16act.mlir"
                                                   : "prefill.mlir";
    const fs::path decoder = in_dir(decoder_name);
    const fs::path prefill = in_dir(prefill_name);
    for (const auto& required : {decoder, prefill}) {
        if (!is_file(required)) {
            throw std::runtime_error(dir.u8string() + " has no amica_app.json, and its graph " +
                                     required.filename().u8string() + " was not found");
        }
    }

    json graphs = {{"prefill", prefill_name}, {"decoder", decoder_name}};
    json manifest = {{"schema_version", 3},
                     {"runtime", "dx12"},
                     {"supported_runtimes", {"dx12", "hip"}},
                     {"source_model_dir", "."},
                     {"graph_variant", bf16_residual ? "bf16_residual_fp16_compute" : qdq ? "qdq_to_fp16act" : "fp16act"},
                     {"amica_optimizations", qdq ? "qdqtofp16act" : ""}};

    // Bundles with several fixed-shape prefill graphs index them by capacity.
    const json variant_index = load_json(in_dir("prefill_variants.json"));
    if (!variant_index.is_null()) {
        if (to_int(variant_index.value("schema_version", json(0)), "schema_version") != 1 ||
            string_value(variant_index, "selection_policy") != "smallest_capacity_covering_prompt") {
            throw std::runtime_error("Unsupported prefill variant descriptor: " + in_dir("prefill_variants.json").u8string());
        }
        const std::int64_t cache_capacity = to_int(variant_index.value("cache_capacity", json(0)), "cache_capacity");
        std::map<std::int64_t, std::string> variants;
        for (const auto& variant : variant_index.value("variants", json::array())) {
            const std::int64_t capacity = to_int(variant.value("capacity", json(0)), "capacity");
            const std::string graph = string_value(variant, bf16_residual ? "bf16_residual" : "fp16act");
            if (capacity <= 0 || graph.empty()) throw std::runtime_error("Invalid prefill variant in prefill_variants.json");
            if (cache_capacity > 0 && capacity > cache_capacity) {
                throw std::runtime_error("Prefill capacity " + std::to_string(capacity) + " exceeds cache capacity " +
                                         std::to_string(cache_capacity));
            }
            if (!is_file(in_dir(graph))) throw std::runtime_error("Prefill variant graph was not found: " + graph);
            if (!variants.emplace(capacity, graph).second) {
                throw std::runtime_error("Duplicate prefill capacity " + std::to_string(capacity));
            }
        }
        if (!variants.empty()) {
            json list = json::array();
            bool has_default = false;
            for (const auto& [capacity, graph] : variants) {
                list.push_back({{"capacity", capacity}, {"graph", graph}});
                has_default = has_default || graph == prefill_name;
            }
            if (!has_default) {
                throw std::runtime_error("The default prefill graph '" + prefill_name + "' is absent from prefill_variants.json");
            }
            graphs["prefill_variants"] = list;
            manifest["prefill_selection"] = {{"policy", "smallest_capacity_covering_prompt"},
                                             {"fallback", "reject_oversize_prompt"},
                                             {"cache_capacity", cache_capacity}};
        }
    }
    manifest["graphs"] = graphs;

    std::string resource;
    for (const char* name : {"weights.safetensors", "model.safetensors"}) {
        if (is_file(in_dir(name))) {
            resource = name;
            break;
        }
    }
    if (resource.empty()) {
        throw std::runtime_error("Neither weights.safetensors nor model.safetensors was found in " + dir.u8string());
    }
    manifest["resources"] = resource;

    // A token-ID input and a logits result make a self-contained causal LM,
    // which owns its embedding and LM head.
    const std::string decoder_path = decoder.u8string();
    const EntryPoint decoder_entry = entry_point(decoder_path);
    const std::string decoder_input_type = first_tensor_type(decoder_entry, decoder_path);
    const std::string decoder_output_type = first_result_type(decoder_entry, decoder_path);
    const bool self_contained = decoder_input_type.find("!cgc.int64>") != std::string::npos &&
                                (ends_with(decoder_output_type, "!cgc.float16>") ||
                                 ends_with(decoder_output_type, "!cgc.float32>"));

    std::string embedding_resource;
    const std::string embedding_tensor = "model.embed_tokens.weight";
    if (self_contained) {
        manifest["decoder_append"] = nullptr;
    } else {
        for (const std::string& candidate : {std::string("_prepared\\model.safetensors"), std::string("model.safetensors"), resource}) {
            if (safetensors_contains(in_dir(candidate), embedding_tensor)) {
                embedding_resource = candidate;
                break;
            }
        }
        if (embedding_resource.empty()) {
            throw std::runtime_error("FP16 embedding tensor '" + embedding_tensor + "' was not found in " + dir.u8string());
        }
        struct HeadCandidate {
            std::string graph;
            std::vector<std::string> resources;
        };
        const std::vector<HeadCandidate> heads = {
            {"_prepared\\lm_head_dense.mlir",
             {"_prepared\\lm_head_dense.safetensors", "_prepared\\model.safetensors", "model.safetensors", resource}},
            {"lm_head_fp16.mlir", {"model.safetensors", "_prepared\\model.safetensors", resource}},
        };
        json head_plan;
        for (const auto& candidate : heads) {
            const fs::path head_graph = in_dir(candidate.graph);
            if (!is_file(head_graph)) continue;
            const std::string head_text = read_text_file(head_graph.u8string());
            // Select by the graph's constant-resource contract, not by file
            // name: a same-stem file can hold a different head's tensors.
            std::string head_resource;
            for (const auto& name : candidate.resources) {
                if (safetensors_satisfies_graph(in_dir(name), head_text)) {
                    head_resource = name;
                    break;
                }
            }
            if (head_resource.empty()) continue;
            const EntryPoint head_entry = parse_entry_point(head_text, head_graph.u8string());
            if (head_entry.input_names.empty()) continue;
            std::string graph_name = candidate.graph;
            if (decoder_output_type != first_tensor_type(head_entry, head_graph.u8string())) {
                const fs::path out_dir = adapter_directory(dir);
                fs::create_directories(out_dir);
                const fs::path adapted = out_dir / "amica_lm_head.mlir";
                std::ofstream file(adapted, std::ios::binary | std::ios::trunc);
                file << adapt_lm_head(head_text, decoder_output_type, head_graph.u8string()) << "\r\n";
                if (!file) throw std::runtime_error("cannot write " + adapted.u8string());
                graph_name = adapted.u8string();
            }
            head_plan = {{"graph", graph_name},
                         {"resources", head_resource},
                         {"input", head_entry.input_names[0]},
                         {"resource_map", entry_resource_map(head_text, head_entry.input_names, in_dir(head_resource))},
                         {"boundary", "decoder_output_gpu_resident"}};
            break;
        }
        if (head_plan.is_null()) {
            throw std::runtime_error("A runnable FP16 LM-head graph was not found in " + dir.u8string());
        }
        manifest["decoder_append"] = head_plan;
    }

    const std::string prefill_path = prefill.u8string();
    const std::string prefill_text = read_text_file(prefill_path);
    const EntryPoint prefill_entry = parse_entry_point(prefill_text, prefill_path);
    const auto& prefill_params = prefill_entry.input_names;
    if (std::any_of(prefill_entry.inputs.begin(), prefill_entry.inputs.end(),
                    [](const std::string& type) { return type == "!cgc.null"; })) {
        throw std::runtime_error("Entry parameter name/type count mismatch in " + prefill_path);
    }
    const auto& decoder_params = decoder_entry.input_names;
    if (prefill_params.size() < 3 || decoder_params.size() < 3) {
        throw std::runtime_error(self_contained ? "Self-contained generation requires token, mask, and cache parameters"
                                                : "Generation requires activation, position, and sequence entry parameters");
    }

    // Prompt formatting does not depend on how the logits are produced.
    const json config = load_json(in_dir("config.json"));
    const std::string model_type = string_value(config, "model_type");
    std::vector<std::string> architectures;
    if (config.is_object() && config.contains("architectures")) architectures = token_contents(config.at("architectures"));
    // ChatApp compares these case-insensitively (PowerShell -eq / -match).
    const std::string lower_model_type = lower(model_type);
    const bool harmony = (lower_model_type == "gpt_oss" || lower_model_type == "gpt-oss" || lower_model_type == "gptoss") ||
                         (lower_model_type == "openai_moe" &&
                          std::any_of(architectures.begin(), architectures.end(), [](const std::string& a) {
                              return lower(a) == "openaimoeforcausallm";
                          }));
    const json tokenizer_config = load_json(in_dir("tokenizer_config.json"));
    bool embedded_template = false;
    bool chat_ml = false;
    const auto has_chat_ml = [](const std::vector<std::string>& tokens) {
        return std::find(tokens.begin(), tokens.end(), "<|im_start|>") != tokens.end() &&
               std::find(tokens.begin(), tokens.end(), "<|im_end|>") != tokens.end();
    };
    if (!tokenizer_config.is_null()) {
        const std::string chat_template = string_value(tokenizer_config, "chat_template");
        embedded_template = chat_template.find_first_not_of(" \t\r\n") != std::string::npos;
        std::vector<std::string> added;
        if (tokenizer_config.contains("added_tokens_decoder") && tokenizer_config.at("added_tokens_decoder").is_object()) {
            for (const auto& [id, value] : tokenizer_config.at("added_tokens_decoder").items()) {
                added.push_back(string_value(value, "content"));
            }
        }
        if (tokenizer_config.contains("extra_special_tokens")) {
            const auto extra = token_contents(tokenizer_config.at("extra_special_tokens"));
            added.insert(added.end(), extra.begin(), extra.end());
        }
        chat_ml = has_chat_ml(added);
    }
    if (!chat_ml && is_file(in_dir("tokenizer.json"))) {
        std::vector<std::string> special;
        for (const auto& token : json::parse(read_text_file(in_dir("tokenizer.json").u8string())).value("added_tokens", json::array())) {
            if (token.value("special", false)) special.push_back(string_value(token, "content"));
        }
        chat_ml = has_chat_ml(special);
    }
    const bool chat_template = harmony || (config.is_object() && lower_model_type == "muse_glimmer") || embedded_template ||
                               is_file(in_dir("chat_template.jinja")) || chat_ml;
    const json generation_config = load_json(in_dir("generation_config.json"));

    json generation;
    std::size_t ple_context = prefill_params.size();
    for (std::size_t i = 0; i < prefill_params.size(); ++i) {
        if (prefill_params[i] == "ple_context_l1") {
            ple_context = i;
            break;
        }
    }
    if (self_contained) {
        if (!config.is_object()) throw std::runtime_error("Self-contained generation requires config.json: " + dir.u8string());
        const json& language = config.contains("text_config") && !config.at("text_config").is_null() ? config.at("text_config") : config;
        if (!language.contains("vocab_size") || language.at("vocab_size").is_null()) {
            throw std::runtime_error("config.json has no language-model vocab_size: " + in_dir("config.json").u8string());
        }
        const bool qwen4_exp_text = lower(string_value(language, "model_type")) == "qwen4_exp_text";
        const bool qwen4_exp = lower_model_type == "qwen4_exp" || qwen4_exp_text;
        std::int64_t pad_token_id = 0;
        if (language.contains("pad_token_id") && !language.at("pad_token_id").is_null()) {
            pad_token_id = to_int(language.at("pad_token_id"), "pad_token_id");
        } else if (qwen4_exp_text && language.contains("eos_token_id") &&
                   !language.at("eos_token_id").is_null()) {
            // PLE's empty rolling context is EOS-initialized, so EOS also pads.
            pad_token_id = to_int(language.at("eos_token_id"), "eos_token_id");
        } else if (generation_config.is_object() && generation_config.contains("pad_token_id") &&
                   !generation_config.at("pad_token_id").is_null()) {
            pad_token_id = to_int(generation_config.at("pad_token_id"), "pad_token_id");
        }
        std::vector<std::string> zero;
        for (std::size_t i = 0; i < prefill_params.size(); ++i) {
            if (i != 0 && i != 1 && i != ple_context) zero.push_back("arg" + std::to_string(i));
        }
        generation = {
            {"self_contained_logits", true},
            {"prompt_format", chat_template ? "chat_template" : "raw"},
            // Qwen4-Exp's prefill advances its recurrent state through the live
            // prefix only, so its prompt sits at the start of the window.
            {"prompt_padding", qwen4_exp ? "right" : "left"},
            {"vocab_size", to_int(language.at("vocab_size"), "vocab_size")},
            {"pad_token_id", pad_token_id},
            // CGC canonicalizes entry parameters positionally (argN), whatever
            // their source names.
            {"prefill_token_input", "arg0"},
            {"prefill_attention_mask_input", "arg1"},
            {"decoder_token_input", "arg0"},
            {"decoder_attention_mask_input", "arg1"},
            {"zero_initialize_inputs", join(zero, ",")},
        };
        if (ple_context < prefill_params.size()) {
            generation["initial_input_fill"] = "arg" + std::to_string(ple_context) + "=" + std::to_string(pad_token_id);
        }
    } else {
        generation = {
            {"prompt_format", chat_template ? "chat_template" : "raw"},
            {"prompt_padding", "left"},
            {"embedding_file", embedding_resource},
            {"embedding_tensor", embedding_tensor},
            {"prefill_embedding_input", prefill_params[0]},
            {"prefill_position_input", prefill_params[1]},
            {"prefill_sequence_input", prefill_params[2]},
            {"decoder_embedding_input", decoder_params[0]},
            {"decoder_position_input", decoder_params[1]},
            {"decoder_sequence_input", decoder_params[2]},
            // The final prefill row includes the chat control tokens, which the
            // LM head must see.
            {"prefill_tail_row_offset", 0},
        };
    }

    const std::string parameter_resources = is_file(in_dir("adapter.safetensors")) ? "adapter.safetensors" : "";
    const std::string parameter_map =
        parameter_resources.empty() ? "" : entry_resource_map(prefill_text, prefill_params, in_dir(parameter_resources));
    if (!parameter_resources.empty() && parameter_map.empty()) {
        throw std::runtime_error("Cannot derive an explicit entry-parameter map for adapter.safetensors in " + prefill_path);
    }
    manifest["parameter_resources"] = parameter_resources;
    manifest["parameter_resource_map"] = parameter_map;

    if (!self_contained) {
        // Recurrent state follows the activation, position and sequence inputs,
        // one per prefill output after the first, and starts at zero.
        std::set<std::string> mapped;
        std::size_t start = 0;
        while (start < parameter_map.size()) {
            std::size_t end = parameter_map.find(',', start);
            if (end == std::string::npos) end = parameter_map.size();
            const std::string entry = parameter_map.substr(start, end - start);
            const std::size_t equals = entry.find('=');
            if (equals != std::string::npos) mapped.insert(entry.substr(0, equals));
            start = end + 1;
        }
        std::size_t results = 0;
        for (const auto& output : prefill_entry.outputs) {
            if (output.rfind("!cgc.tensor<", 0) == 0) ++results;
        }
        const std::size_t states = results > 0 ? results - 1 : 0;
        const std::size_t first_non_state = std::min(prefill_params.size(), 3 + states);
        std::vector<std::string> zero;
        for (std::size_t i = 3; i < first_non_state; ++i) {
            if (!mapped.count(prefill_params[i])) zero.push_back(prefill_params[i]);
        }
        // Quantized exports' symmetric zero points arrive as INT8/UINT8 inputs
        // after the state range and must be zero too.
        for (std::size_t i = first_non_state; i < prefill_params.size(); ++i) {
            const std::string& type = prefill_entry.inputs[i];
            if (!mapped.count(prefill_params[i]) && (ends_with(type, "!cgc.int8>") || ends_with(type, "!cgc.uint8>"))) {
                zero.push_back(prefill_params[i]);
            }
        }
        if (!zero.empty()) generation["zero_initialize_inputs"] = join(zero, ",");
    }
    manifest["generation"] = generation;
    return manifest;
}

}  // namespace amica_engine
