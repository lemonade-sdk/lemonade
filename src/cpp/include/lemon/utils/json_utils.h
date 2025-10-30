#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace lemon {
namespace utils {

using json = nlohmann::json;

class JsonUtils {
public:
    // Load JSON from file
    static json load_from_file(const std::string& file_path);
    
    // Save JSON to file
    static void save_to_file(const json& j, const std::string& file_path);
    
    // Parse JSON from string
    static json parse(const std::string& json_str);
    
    // Convert JSON to pretty string
    static std::string to_string(const json& j, int indent = 2);
    
    // Merge two JSON objects
    static json merge(const json& base, const json& overlay);
    
    // Check if JSON has key
    static bool has_key(const json& j, const std::string& key);
    
    // Get value with default
    template<typename T>
    static T get_or_default(const json& j, const std::string& key, const T& default_value) {
        if (j.contains(key) && !j[key].is_null()) {
            return j[key].get<T>();
        }
        return default_value;
    }
};

} // namespace utils
} // namespace lemon

