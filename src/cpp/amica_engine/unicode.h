#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct URegularExpression;

namespace amica_engine {

// The pattern GPT-2's byte-level pre-tokenizer splits with.
extern const char* const kByteLevelPattern;

std::string nfc(const std::string& text);

// Full Unicode lowercase mapping in the root locale, as Python's str.lower().
std::string lowercase(const std::string& text);

// tokenizers matches \s and \S against Unicode White_Space; ICU's \s omits a
// few of its characters (U+000B, U+0085), so they are spelled out.
std::string to_icu_pattern(const std::string& pattern);

void append_utf8(std::string& out, std::uint32_t code_point);

// Decodes one code point of valid UTF-8 starting at text[i] and advances i.
std::uint32_t next_code_point(const std::string& text, std::size_t& i);

bool is_whitespace(std::uint32_t code_point);

// A compiled ICU regular expression.
class Regex {
public:
    Regex(const std::string& pattern, bool literal);
    ~Regex();
    Regex(const Regex&) = delete;
    Regex& operator=(const Regex&) = delete;

    // Pieces of text with whether each is a pattern match, in order.
    std::vector<std::pair<std::string, bool>> split(const std::string& text) const;

private:
    URegularExpression* pattern_ = nullptr;
};

}  // namespace amica_engine
