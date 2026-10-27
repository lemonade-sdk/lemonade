#include "bundle.h"

#include "app_manifest.h"
#include "graph_abi.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace amica_engine {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// Large tensors stay out of the compiled graphs, so prefill and decoder bind
// one copy of each instead of embedding their own.
constexpr std::uint64_t kExternalWeightsMinBytes = 1ull << 20;

bool truthy(const json& value) {
    if (value.is_null()) return false;
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number()) return value.get<double>() != 0.0;
    if (value.is_string()) return !value.get<std::string>().empty();
    if (value.is_array() || value.is_object()) return !value.empty();
    return true;
}

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// Binds prefill cache outputs to the decoder inputs with identical types, as
// the ChatApp runner does: by an exact type sequence rather than by position,
// because some graphs place weights after the runtime inputs and others append
// non-cache outputs after the caches.
std::pair<json, std::vector<std::string>> cache_output_map(const std::string& prefill, const std::string& decoder) {
    const auto prefill_outputs = entry_point(prefill).outputs;
    const auto decoder_inputs = entry_point(decoder).inputs;
    const std::vector<std::string> candidates(prefill_outputs.begin() + 1, prefill_outputs.end());
    for (std::size_t count = std::min(candidates.size(), decoder_inputs.size()); count > 0; --count) {
        for (std::size_t start = 0; start + count <= decoder_inputs.size(); ++start) {
            if (std::equal(candidates.begin(), candidates.begin() + count, decoder_inputs.begin() + start)) {
                json mapping = json::object();
                for (std::size_t index = 0; index < count; ++index) {
                    mapping["main:#output_" + std::to_string(index + 1)] = "arg" + std::to_string(start + index);
                }
                return {mapping, std::vector<std::string>(candidates.begin(), candidates.begin() + count)};
            }
        }
    }
    throw std::runtime_error("No decoder input sequence matches the prefill cache outputs of " +
                             fs::u8path(prefill).filename().u8string());
}

// "name=value,name=value" lists, as the manifests write resource maps.
std::map<std::string, std::string> parse_map(const json& value) {
    std::map<std::string, std::string> result;
    if (!value.is_string()) return result;
    std::stringstream entries(value.get<std::string>());
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        if (trim(entry).empty()) continue;
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos || equals + 1 == entry.size()) {
            throw std::runtime_error("Invalid resource_map entry '" + entry + "'");
        }
        result[trim(entry.substr(0, equals))] = trim(entry.substr(equals + 1));
    }
    return result;
}

std::string path_string(const fs::path& path) { return path.u8string(); }

}  // namespace

Bundle::Bundle(const std::string& model_dir) {
    const fs::path dir = fs::u8path(model_dir);
    const fs::path manifest_path = dir / "amica_app.json";
    manifest_path_ = path_string(manifest_path);
    if (fs::is_regular_file(manifest_path)) {
        std::string text = read_text_file(manifest_path_);
        // PowerShell writes the manifest with a byte order mark.
        if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
        manifest_ = json::parse(text);
        manifest_derived_ = false;
    } else {
        manifest_ = derive_app_manifest(model_dir);
        manifest_derived_ = true;
    }

    source_dir_ = path_string(fs::weakly_canonical(dir / fs::u8path(manifest_.value("source_model_dir", "."))));
    generation_ = manifest_.value("generation", json());
    if (!truthy(generation_)) {
        throw std::runtime_error(manifest_path_ + " has no generation metadata");
    }
    self_contained_ = generation_.value("self_contained_logits", json()).is_boolean() &&
                      generation_.at("self_contained_logits").get<bool>();
    if (self_contained_) {
        const json version = manifest_.value("schema_version", json(0));
        const long long schema = version.is_number() ? version.get<long long>()
                                 : version.is_string() ? std::stoll(version.get<std::string>())
                                                       : 0;
        if (schema < 3 || !generation_.contains("zero_initialize_inputs")) {
            throw std::runtime_error(manifest_path_ +
                                     " needs a schema-3 input plan; re-run the ChatApp preparation for this bundle");
        }
    }
    if (!self_contained_ && !truthy(manifest_.value("decoder_append", json()))) {
        throw std::runtime_error(manifest_path_ + " has no LM-head graph");
    }
    for (const char* unsupported : {"amica_optimizations", "parameter_resources"}) {
        if (truthy(manifest_.value(unsupported, json()))) {
            throw std::runtime_error(manifest_path_ + " sets " + unsupported +
                                     ", which the public AMICA API used by amica-engine cannot apply");
        }
    }
    const std::string runtime = manifest_.value("runtime", "dx12");
    const json supported = manifest_.value("supported_runtimes", json::array());
    if (runtime != "dx12" && std::find(supported.begin(), supported.end(), "dx12") == supported.end()) {
        throw std::runtime_error("AMICA ChatApp generation requires a DX12 bundle");
    }

    decoder_ = path_string(fs::u8path(source_dir_) / fs::u8path(manifest_.at("graphs").at("decoder").get<std::string>()));
    const std::string padding = generation_.value("prompt_padding", json()).is_string()
                                    ? generation_.at("prompt_padding").get<std::string>()
                                    : "";
    prompt_padding_ = padding == "left" || padding == "right" ? padding : "right";
    chat_template_ = generation_.value("prompt_format", json()) == "chat_template";

    load_prefill_graphs();
    if (self_contained_) {
        const auto decoder_inputs = entry_point(decoder_).inputs;
        std::size_t elements = 1;
        const std::size_t mask = parameter_index(generation_.at("decoder_attention_mask_input").get<std::string>());
        for (const std::size_t dim : tensor_dims(decoder_inputs.at(mask))) elements *= dim;
        context_capacity_ = elements;
    } else {
        // A [batch, heads, sequence, dim] cache gives the context length.
        const auto dims = tensor_dims(cache_output_map(prefill_.back().graph, decoder_).second.at(0));
        if (dims.size() == 4) context_capacity_ = dims[2];
    }
    build_plan();
}

std::string Bundle::prefill_input() const {
    return generation_.at(self_contained_ ? "prefill_token_input" : "prefill_embedding_input").get<std::string>();
}

std::size_t Bundle::graph_capacity(const std::string& graph) const {
    const auto inputs = entry_point(graph).inputs;
    const auto dims = tensor_dims(inputs.at(parameter_index(prefill_input())));
    if (dims.size() < 2) {
        throw std::runtime_error("Cannot determine the prefill capacity of " + graph);
    }
    return dims[1];
}

void Bundle::load_prefill_graphs() {
    const fs::path source = fs::u8path(source_dir_);
    std::vector<std::pair<std::string, std::size_t>> entries;
    const json variants = manifest_.at("graphs").value("prefill_variants", json());
    bool any_variant = false;
    if (variants.is_array()) {
        for (const auto& variant : variants) {
            if (!truthy(variant)) continue;
            any_variant = true;
            entries.emplace_back(path_string(source / fs::u8path(variant.at("graph").get<std::string>())),
                                 variant.at("capacity").get<std::size_t>());
        }
    }
    if (any_variant) {
        const json selection = manifest_.value("prefill_selection", json());
        const json policy = selection.is_object() ? selection.value("policy", json()) : json();
        if (truthy(policy) && policy != "smallest_capacity_covering_prompt") {
            throw std::runtime_error("Unsupported prefill selection policy: " + policy.dump());
        }
    } else {
        const std::string graph = path_string(source / fs::u8path(manifest_.at("graphs").at("prefill").get<std::string>()));
        entries.emplace_back(graph, graph_capacity(graph));
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& [graph, capacity] : entries) {
        prefill_.push_back({graph, capacity, cache_output_map(graph, decoder_).first});
    }
}

void Bundle::build_plan() {
    const json& g = generation_;
    const auto name = [&](const char* key) { return g.value(key, json()); };
    const auto either = [&](const char* key, const char* fallback) {
        return g.contains(key) ? g.at(key) : name(fallback);
    };

    json zero_initialize = json::array();
    {
        const json list = name("zero_initialize_inputs");
        std::stringstream inputs(list.is_string() ? list.get<std::string>() : "");
        std::string input;
        while (std::getline(inputs, input, ',')) {
            if (!trim(input).empty()) zero_initialize.push_back(trim(input));
        }
    }
    json fills = json::object();
    for (const auto& [input, value] : parse_map(name("initial_input_fill"))) {
        fills[input] = std::stoll(value, nullptr, 0);
    }
    json prefill = json::array();
    for (const auto& variant : prefill_) {
        prefill.push_back({{"graph", variant.graph}, {"capacity", variant.capacity}, {"cache_map", variant.cache_map}});
    }
    const fs::path source = fs::u8path(source_dir_);
    plan_ = {
        {"logits", self_contained_ ? "graph" : "lm_head"},
        {"prompt_padding", prompt_padding_},
        {"prefill", prefill},
        {"decoder", decoder_},
        {"resources", path_string(source / fs::u8path(manifest_.at("resources").get<std::string>()))},
        {"external_weights_min_bytes", kExternalWeightsMinBytes},
        {"inputs",
         {{"prefill_position", either("prefill_position_input", "decoder_position_input")},
          {"prefill_sequence", either("prefill_sequence_input", "decoder_sequence_input")},
          {"decoder_position", name("decoder_position_input")},
          {"decoder_sequence", name("decoder_sequence_input")}}},
        {"zero_initialize", zero_initialize},
        {"initial_input_fill", fills},
    };
    if (self_contained_) {
        plan_["vocab_size"] = g.at("vocab_size").get<std::int64_t>();
        plan_["pad_token_id"] = name("pad_token_id").is_number_integer() ? name("pad_token_id").get<std::int64_t>() : 0;
        plan_["inputs"]["prefill_token"] = g.at("prefill_token_input");
        plan_["inputs"]["prefill_attention_mask"] = g.at("prefill_attention_mask_input");
        plan_["inputs"]["decoder_token"] = g.at("decoder_token_input");
        plan_["inputs"]["decoder_attention_mask"] = g.at("decoder_attention_mask_input");
        return;
    }
    const json& tail = manifest_.at("decoder_append");
    json resource_map = json::object();
    for (const auto& [input, tensor] : parse_map(tail.value("resource_map", json()))) resource_map[input] = tensor;
    plan_["lm_head"] = {{"graph", path_string(source / fs::u8path(tail.at("graph").get<std::string>()))},
                        {"resources", path_string(source / fs::u8path(tail.at("resources").get<std::string>()))},
                        {"resource_map", resource_map}};
    // amica-driver binds the LM head through the decoder's embedding parameter
    // name; the manifests rely on that convention.
    plan_["tail_input"] = g.at("decoder_embedding_input");
    plan_["tail_row_offset"] =
        name("prefill_tail_row_offset").is_number_integer() ? name("prefill_tail_row_offset").get<std::int64_t>() : 0;
    plan_["embedding"] = {{"file", path_string(source / fs::u8path(g.at("embedding_file").get<std::string>()))},
                          {"tensor", g.at("embedding_tensor")}};
    plan_["inputs"]["prefill_embedding"] = g.at("prefill_embedding_input");
    plan_["inputs"]["decoder_embedding"] = g.at("decoder_embedding_input");
}

}  // namespace amica_engine
