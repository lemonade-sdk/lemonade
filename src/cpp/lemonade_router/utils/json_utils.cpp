#include <lemon/utils/json_utils.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <iostream>

namespace lemon {
namespace utils {

json JsonUtils::load_from_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + file_path);
    }
    
    json j;
    try {
        file >> j;
    } catch (const json::exception& e) {
        throw std::runtime_error("Failed to parse JSON from file " + file_path + ": " + e.what());
    }
    
    return j;
}

void JsonUtils::save_to_file(const json& j, const std::string& file_path) {
    std::ofstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file for writing: " + file_path);
    }
    
    try {
        file << j.dump(2);  // Pretty print with 2 space indent
    } catch (const json::exception& e) {
        throw std::runtime_error("Failed to write JSON to file " + file_path + ": " + e.what());
    }
}

json JsonUtils::parse(const std::string& json_str) {
    try {
        return json::parse(json_str);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("Failed to parse JSON string: ") + e.what());
    }
}

std::string JsonUtils::to_string(const json& j, int indent) {
    return j.dump(indent);
}

json JsonUtils::merge(const json& base, const json& overlay) {
    json result = base;
    
    if (!overlay.is_object()) {
        return overlay;
    }
    
    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        if (result.contains(it.key()) && result[it.key()].is_object() && it.value().is_object()) {
            result[it.key()] = merge(result[it.key()], it.value());
        } else {
            result[it.key()] = it.value();
        }
    }
    
    return result;
}

bool JsonUtils::has_key(const json& j, const std::string& key) {
    return j.contains(key) && !j[key].is_null();
}

} // namespace utils
} // namespace lemon

