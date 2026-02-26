#include <lemon/utils/json_utils.h>
#include <fstream>
#include <stdexcept>
#include <vector>

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

std::string JsonUtils::base64_encode(const std::string& input) {
    static const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);

    int val = 0;
    int valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            output.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        output.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (output.size() % 4) {
        output.push_back('=');
    }
    return output;
}

std::string JsonUtils::base64_decode(const std::string& input) {
    static const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    static const auto lookup = []() {
        std::vector<int> tbl(256, -1);
        for (int i = 0; i < 64; i++) {
            tbl[static_cast<unsigned char>(base64_chars[i])] = i;
        }
        return tbl;
    }();

    std::string output;
    int val = 0;
    int valb = -8;
    for (unsigned char c : input) {
        if (lookup[c] == -1) break;
        val = (val << 6) + lookup[c];
        valb += 6;
        if (valb >= 0) {
            output.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return output;
}

} // namespace utils
} // namespace lemon
