#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace amica_engine {

// The signature of a CGC graph's entry point.
struct EntryPoint {
    std::vector<std::string> input_names;  // SSA names without '%'
    std::vector<std::string> inputs;       // "!cgc.tensor<...>" or "!cgc.null", without whitespace
    std::vector<std::string> outputs;
};

EntryPoint entry_point(const std::string& graph);

// The same, from graph text already read; path is for error messages.
EntryPoint parse_entry_point(const std::string& text, const std::string& path);

std::vector<std::size_t> tensor_dims(const std::string& type);

// The N of an "argN" parameter name.
std::size_t parameter_index(const std::string& name);

// Tensor names the graph reads from its resources (#cgc.constant_resource<NAME : ...>).
std::vector<std::string> constant_resource_names(const std::string& graph_text);

// Tensor names in a safetensors file's header.
std::vector<std::string> safetensors_tensor_names(const std::string& path);

std::string read_text_file(const std::string& path);

}  // namespace amica_engine
