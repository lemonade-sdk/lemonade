#include "unicode.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <icu.h>

#include <memory>
#include <stdexcept>

namespace amica_engine {

const char* const kByteLevelPattern =
    R"('s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+)";

namespace {

using UString = std::basic_string<UChar>;

void check_icu(UErrorCode status, const char* what) {
    if (U_FAILURE(status)) {
        throw std::runtime_error(std::string(what) + " failed: " + u_errorName(status));
    }
}

UString to_utf16(const std::string& text) {
    UErrorCode status = U_ZERO_ERROR;
    std::int32_t length = 0;
    u_strFromUTF8(nullptr, 0, &length, text.data(), static_cast<std::int32_t>(text.size()), &status);
    if (status != U_BUFFER_OVERFLOW_ERROR) check_icu(status, "u_strFromUTF8");
    UString result(static_cast<std::size_t>(length), 0);
    status = U_ZERO_ERROR;
    u_strFromUTF8(result.data(), length, &length, text.data(), static_cast<std::int32_t>(text.size()), &status);
    check_icu(status, "u_strFromUTF8");
    return result;
}

std::string to_utf8(const UChar* text, std::int32_t size) {
    UErrorCode status = U_ZERO_ERROR;
    std::int32_t length = 0;
    u_strToUTF8(nullptr, 0, &length, text, size, &status);
    if (status != U_BUFFER_OVERFLOW_ERROR) check_icu(status, "u_strToUTF8");
    std::string result(static_cast<std::size_t>(length), '\0');
    status = U_ZERO_ERROR;
    u_strToUTF8(result.data(), length, &length, text, size, &status);
    check_icu(status, "u_strToUTF8");
    return result;
}

}  // namespace

std::string nfc(const std::string& text) {
    UErrorCode status = U_ZERO_ERROR;
    const UNormalizer2* normalizer = unorm2_getNFCInstance(&status);
    check_icu(status, "unorm2_getNFCInstance");
    const UString source = to_utf16(text);
    UString result(source.size() + 16, 0);
    std::int32_t length = unorm2_normalize(normalizer, source.data(), static_cast<std::int32_t>(source.size()),
                                           result.data(), static_cast<std::int32_t>(result.size()), &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        status = U_ZERO_ERROR;
        result.assign(static_cast<std::size_t>(length), 0);
        length = unorm2_normalize(normalizer, source.data(), static_cast<std::int32_t>(source.size()),
                                  result.data(), length, &status);
    }
    check_icu(status, "unorm2_normalize");
    return to_utf8(result.data(), length);
}

std::string lowercase(const std::string& text) {
    const UString source = to_utf16(text);
    UErrorCode status = U_ZERO_ERROR;
    UString result(source.size() + 16, 0);
    std::int32_t length = u_strToLower(result.data(), static_cast<std::int32_t>(result.size()), source.data(),
                                       static_cast<std::int32_t>(source.size()), "", &status);
    if (status == U_BUFFER_OVERFLOW_ERROR) {
        status = U_ZERO_ERROR;
        result.assign(static_cast<std::size_t>(length), 0);
        length = u_strToLower(result.data(), length, source.data(), static_cast<std::int32_t>(source.size()), "",
                              &status);
    }
    check_icu(status, "u_strToLower");
    return to_utf8(result.data(), length);
}

std::string to_icu_pattern(const std::string& pattern) {
    std::string result;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '\\' && i + 1 < pattern.size()) {
            const char next = pattern[++i];
            if (next == 's') {
                result += "\\p{White_Space}";
            } else if (next == 'S') {
                result += "\\P{White_Space}";
            } else {
                result += '\\';
                result += next;
            }
        } else {
            result += pattern[i];
        }
    }
    return result;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

std::uint32_t next_code_point(const std::string& text, std::size_t& i) {
    const auto byte = [&](std::size_t k) { return static_cast<std::uint8_t>(text[k]); };
    const std::uint8_t lead = byte(i);
    if (lead < 0x80) {
        ++i;
        return lead;
    }
    const int length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
    std::uint32_t code_point = lead & (0x7F >> length);
    for (int k = 1; k < length && i + k < text.size(); ++k) {
        code_point = (code_point << 6) | (byte(i + k) & 0x3F);
    }
    i += length;
    return code_point;
}

bool is_whitespace(std::uint32_t code_point) { return u_isUWhiteSpace(static_cast<UChar32>(code_point)) != 0; }

Regex::Regex(const std::string& pattern, bool literal) {
    const UString pattern16 = to_utf16(pattern);
    UParseError parse_error{};
    UErrorCode status = U_ZERO_ERROR;
    pattern_ = uregex_open(pattern16.data(), static_cast<std::int32_t>(pattern16.size()), literal ? UREGEX_LITERAL : 0,
                           &parse_error, &status);
    check_icu(status, "compiling a pre-tokenizer pattern");
}

Regex::~Regex() {
    if (pattern_) uregex_close(pattern_);
}

std::vector<std::pair<std::string, bool>> Regex::split(const std::string& piece) const {
    const UString text = to_utf16(piece);
    UErrorCode status = U_ZERO_ERROR;
    // Each split matches on its own clone, so one Regex serves concurrent requests.
    std::unique_ptr<URegularExpression, decltype(&uregex_close)> regex(uregex_clone(pattern_, &status),
                                                                       &uregex_close);
    check_icu(status, "uregex_clone");
    uregex_setText(regex.get(), text.data(), static_cast<std::int32_t>(text.size()), &status);
    check_icu(status, "uregex_setText");
    std::vector<std::pair<std::string, bool>> parts;
    std::int32_t position = 0;
    while (uregex_findNext(regex.get(), &status)) {
        const std::int32_t start = uregex_start(regex.get(), 0, &status);
        const std::int32_t end = uregex_end(regex.get(), 0, &status);
        check_icu(status, "uregex_findNext");
        if (end == start) continue;
        if (start > position) parts.emplace_back(to_utf8(text.data() + position, start - position), false);
        parts.emplace_back(to_utf8(text.data() + start, end - start), true);
        position = end;
    }
    check_icu(status, "uregex_findNext");
    if (position < static_cast<std::int32_t>(text.size())) {
        parts.emplace_back(to_utf8(text.data() + position, static_cast<std::int32_t>(text.size()) - position), false);
    }
    return parts;
}

}  // namespace amica_engine
