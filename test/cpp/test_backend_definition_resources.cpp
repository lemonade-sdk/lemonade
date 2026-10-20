#include "backend_definition_resource_files.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

int checks = 0;
int failures = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool valid_utf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xe ? 3 : (lead >> 3) == 0x1e ? 4 : 0;
        if (length == 0 || i + length > text.size()) return false;
        for (std::size_t k = 1; k < length; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) >> 6) != 0x2) return false;
        }
        i += length;
    }
    return true;
}

bool identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::optional<std::vector<std::size_t>> literal_sizes(const std::string& text) {
    std::vector<std::size_t> sizes;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, 2, "//") == 0) {
            i = text.find('\n', i);
        } else if (text.compare(i, 2, "/*") == 0) {
            i = text.find("*/", i + 2);
            if (i != std::string::npos) i += 2;
        } else if (text.compare(i, 2, "R\"") == 0 && (i == 0 || !identifier_char(text[i - 1]))) {
            const auto open = text.find('(', i + 2);
            if (open == std::string::npos) return std::nullopt;
            const auto terminator = ")" + text.substr(i + 2, open - i - 2) + "\"";
            const auto close = text.find(terminator, open + 1);
            if (close == std::string::npos) return std::nullopt;
            sizes.push_back(close - open - 1);
            i = close + terminator.size();
        } else if (text[i] == '"' || text[i] == '\'') {
            const char quote = text[i];
            std::size_t end = i + 1;
            while (end < text.size() && text[end] != quote) end += text[end] == '\\' ? 2 : 1;
            if (end >= text.size()) return std::nullopt;
            if (quote == '"') sizes.push_back(end - i - 1);
            i = end + 1;
        } else {
            ++i;
        }
    }
    return sizes;
}

}

int main() {
    for (const auto& resource : lemon::backends::k_embedded_resource_files) {
        const std::string embedded(resource.data, resource.size);
        check(embedded == read_bytes(resource.path), std::string("the embedded constant equals ") + resource.path + " byte for byte");
        check(std::strlen(resource.data) == resource.size, std::string("the embedded constant for ") + resource.path + " has no NUL byte");
    }
    std::size_t literals = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(LEMON_GENERATED_INCLUDE_DIR)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".h") continue;
        const auto name = entry.path().filename().string();
        const auto text = read_bytes(entry.path());
        check(valid_utf8(text), name + " is valid UTF-8");
        const auto sizes = literal_sizes(text);
        check(sizes.has_value(), name + " has no unterminated string literal");
        if (!sizes || sizes->empty()) continue;
        literals += sizes->size();
        const auto longest = *std::max_element(sizes->begin(), sizes->end());
        check(longest <= LEMON_CXX_LITERAL_BUDGET, name + ": the longest string literal is " + std::to_string(longest) + " bytes, over the " + std::to_string(LEMON_CXX_LITERAL_BUDGET) + "-byte budget");
    }
    check(literals > 0, "the generated headers contain string literals to measure");
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
