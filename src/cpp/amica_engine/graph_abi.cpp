#include "graph_abi.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace amica_engine {

namespace {

std::string without_whitespace(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); }),
               text.end());
    return text;
}

bool name_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || std::strchr("_.$-", c); }

// Reads "!cgc.tensor<...>" or "!cgc.null" at text[i]; returns its length, or 0.
std::size_t match_type(const std::string& text, std::size_t i) {
    static const std::string kTensor = "!cgc.tensor<";
    static const std::string kNull = "!cgc.null";
    if (text.compare(i, kTensor.size(), kTensor) == 0) {
        const std::size_t close = text.find('>', i + kTensor.size());
        if (close == std::string::npos || close == i + kTensor.size()) return 0;
        return close + 1 - i;
    }
    if (text.compare(i, kNull.size(), kNull) == 0) return kNull.size();
    return 0;
}

}  // namespace

std::string read_text_file(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path);
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

EntryPoint entry_point(const std::string& graph) { return parse_entry_point(read_text_file(graph), graph); }

EntryPoint parse_entry_point(const std::string& text, const std::string& path) {
    const auto fail = [&]() -> EntryPoint { throw std::runtime_error("Cannot read entry-point ABI: " + path); };
    const std::size_t keyword = text.find("cgc.entry_point");
    if (keyword == std::string::npos) return fail();
    const std::size_t open = text.find('(', text.find('@', keyword));
    if (open == std::string::npos) return fail();
    // The parameter list ends at the first ')' followed by "->".
    std::size_t close = text.find(')', open + 1);
    std::size_t arrow = std::string::npos;
    while (close != std::string::npos) {
        std::size_t k = close + 1;
        while (k < text.size() && std::isspace(static_cast<unsigned char>(text[k]))) ++k;
        if (text.compare(k, 2, "->") == 0) {
            arrow = k + 2;
            break;
        }
        close = text.find(')', close + 1);
    }
    if (arrow == std::string::npos) return fail();
    const std::size_t body = text.find('{', arrow);
    if (body == std::string::npos) return fail();

    EntryPoint result;
    const std::string arguments = text.substr(open + 1, close - open - 1);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] != '%') continue;
        std::size_t k = i + 1;
        while (k < arguments.size() && name_char(arguments[k])) ++k;
        if (k == i + 1) continue;
        const std::string name = arguments.substr(i + 1, k - i - 1);
        while (k < arguments.size() && std::isspace(static_cast<unsigned char>(arguments[k]))) ++k;
        if (k >= arguments.size() || arguments[k] != ':') continue;
        ++k;
        while (k < arguments.size() && std::isspace(static_cast<unsigned char>(arguments[k]))) ++k;
        const std::size_t length = match_type(arguments, k);
        if (length == 0) continue;
        result.input_names.push_back(name);
        result.inputs.push_back(without_whitespace(arguments.substr(k, length)));
        i = k + length - 1;
    }
    const std::string results = text.substr(arrow, body - arrow);
    for (std::size_t i = 0; i < results.size(); ++i) {
        const std::size_t length = match_type(results, i);
        if (length == 0) continue;
        result.outputs.push_back(without_whitespace(results.substr(i, length)));
        i += length - 1;
    }
    if (result.inputs.empty() || result.outputs.empty()) {
        throw std::runtime_error("Entry-point ABI has no inputs or outputs: " + path);
    }
    return result;
}

std::vector<std::size_t> tensor_dims(const std::string& type) {
    const std::size_t open = type.find('<');
    const std::size_t close = type.rfind('>');
    std::vector<std::size_t> dims;
    if (open == std::string::npos || close == std::string::npos || close <= open) return dims;
    std::stringstream inner(type.substr(open + 1, close - open - 1));
    std::string part;
    while (std::getline(inner, part, 'x')) {
        if (!part.empty() && std::all_of(part.begin(), part.end(), [](unsigned char c) { return std::isdigit(c); })) {
            dims.push_back(std::stoull(part));
        }
    }
    return dims;
}

std::size_t parameter_index(const std::string& name) {
    if (name.size() < 4 || name.compare(0, 3, "arg") != 0 ||
        !std::all_of(name.begin() + 3, name.end(), [](unsigned char c) { return std::isdigit(c); })) {
        throw std::runtime_error("Expected an argN graph parameter name, got '" + name + "'");
    }
    return std::stoull(name.substr(3));
}

std::vector<std::string> constant_resource_names(const std::string& graph_text) {
    static const std::string kResource = "#cgc.constant_resource<";
    std::vector<std::string> names;
    for (std::size_t at = graph_text.find(kResource); at != std::string::npos;
         at = graph_text.find(kResource, at + 1)) {
        std::size_t k = at + kResource.size();
        while (k < graph_text.size() && std::isspace(static_cast<unsigned char>(graph_text[k]))) ++k;
        const std::size_t begin = k;
        while (k < graph_text.size() && !std::isspace(static_cast<unsigned char>(graph_text[k])) &&
               graph_text[k] != ':' && graph_text[k] != '>') {
            ++k;
        }
        std::size_t colon = k;
        while (colon < graph_text.size() && std::isspace(static_cast<unsigned char>(graph_text[colon]))) ++colon;
        if (k == begin || colon >= graph_text.size() || graph_text[colon] != ':') continue;
        const std::string name = graph_text.substr(begin, k - begin);
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return names;
}

std::vector<std::string> safetensors_tensor_names(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    std::uint64_t length = 0;
    if (!file || !file.read(reinterpret_cast<char*>(&length), sizeof(length))) {
        throw std::runtime_error("Cannot read safetensors header length: " + path);
    }
    if (length == 0 || length > (64ull << 20)) {
        throw std::runtime_error("Invalid safetensors header length: " + path);
    }
    std::string header(static_cast<std::size_t>(length), '\0');
    if (!file.read(header.data(), static_cast<std::streamsize>(length))) {
        throw std::runtime_error("Cannot read safetensors header: " + path);
    }
    const auto tensors = nlohmann::ordered_json::parse(header);
    std::vector<std::string> names;
    for (const auto& [name, value] : tensors.items()) {
        if (name != "__metadata__") names.push_back(name);
    }
    return names;
}

}  // namespace amica_engine
