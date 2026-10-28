#include "lemon/backends/amica/amica_bundle.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lemon {
namespace backends {
namespace amica {

namespace {

std::optional<std::string> read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    return text.substr(begin, text.find_last_not_of(" \t\r\n") - begin + 1);
}

std::vector<std::string> entry_input_types(const std::string& text) {
    const std::size_t keyword = text.find("cgc.entry_point");
    if (keyword == std::string::npos) return {};
    const std::size_t open = text.find('(', keyword);
    if (open == std::string::npos) return {};

    std::vector<std::string> params;
    std::string current;
    int parens = 0;
    int angles = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(' && parens++ == 0) continue;
        if (c == ')' && --parens == 0) {
            if (!trim(current).empty()) params.push_back(current);
            break;
        }
        if (c == '<') ++angles;
        if (c == '>') --angles;
        if (c == ',' && angles == 0 && parens == 1) {
            params.push_back(current);
            current.clear();
            continue;
        }
        current += c;
    }
    if (parens != 0) return {};

    std::vector<std::string> types;
    for (const auto& param : params) {
        const std::size_t colon = param.find(':');
        if (colon == std::string::npos) return {};
        types.push_back(trim(param.substr(colon + 1)));
    }
    return types;
}

std::vector<int64_t> tensor_dims(const std::string& type) {
    static const std::string kPrefix = "!cgc.tensor<";
    if (type.rfind(kPrefix, 0) != 0) return {};
    std::vector<int64_t> dims;
    std::istringstream shape(type.substr(kPrefix.size()));
    std::string token;
    while (std::getline(shape, token, 'x')) {
        if (token.empty() || !std::all_of(token.begin(), token.end(),
                                          [](unsigned char c) { return std::isdigit(c); })) {
            break;
        }
        dims.push_back(std::stoll(token));
    }
    return dims;
}

// CGC numbers entry parameters positionally, whatever their source names.
std::optional<std::size_t> parameter_index(const std::string& name) {
    if (name.size() < 4 || name.compare(0, 3, "arg") != 0) return std::nullopt;
    for (std::size_t i = 3; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return std::nullopt;
    }
    return static_cast<std::size_t>(std::stoull(name.substr(3)));
}

}  // namespace

std::optional<int64_t> bundle_context_length(const std::string& model_dir) {
    try {
        const fs::path dir = fs::u8path(model_dir);
        fs::path decoder;
        std::optional<bool> self_contained;
        std::string mask_input = "arg1";

        // A bundle's precision variants share one shape, so any decoder will do.
        if (const auto app = read_file(dir / "amica_app.json")) {
            std::string text = *app;
            if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
            const json manifest = json::parse(text);
            const std::string source = manifest.value("source_model_dir", ".");
            decoder = dir / fs::u8path(source) /
                      fs::u8path(manifest.at("graphs").at("decoder").get<std::string>());
            const json generation = manifest.value("generation", json::object());
            const json flag = generation.value("self_contained_logits", json());
            self_contained = flag.is_boolean() && flag.get<bool>();
            const json mask = generation.value("decoder_attention_mask_input", json());
            if (mask.is_string()) mask_input = mask.get<std::string>();
        } else {
            for (const char* name : {"decoder_bf16residual.mlir", "decoder_fp16act.mlir", "decoder.mlir"}) {
                if (fs::is_regular_file(dir / name)) {
                    decoder = dir / name;
                    break;
                }
            }
        }
        if (decoder.empty()) return std::nullopt;

        const auto text = read_file(decoder);
        if (!text) return std::nullopt;
        const auto inputs = entry_input_types(*text);
        if (inputs.empty()) return std::nullopt;

        if (!self_contained) {
            static const std::string kInt64 = "!cgc.int64>";
            const std::string& first = inputs[0];
            self_contained = first.size() >= kInt64.size() &&
                             first.compare(first.size() - kInt64.size(), kInt64.size(), kInt64) == 0;
        }

        if (*self_contained) {
            const auto mask = parameter_index(mask_input);
            if (!mask || *mask >= inputs.size()) return std::nullopt;
            const auto dims = tensor_dims(inputs[*mask]);
            if (dims.empty()) return std::nullopt;
            int64_t elements = 1;
            for (const int64_t dim : dims) elements *= dim;
            return elements > 0 ? std::optional<int64_t>(elements) : std::nullopt;
        }
        for (const auto& input : inputs) {
            const auto dims = tensor_dims(input);
            if (dims.size() == 4 && dims[2] > 0) return dims[2];
        }
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace amica
}  // namespace backends
}  // namespace lemon
