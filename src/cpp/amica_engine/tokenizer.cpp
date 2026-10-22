#include "tokenizer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <icu.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace amica_engine {

namespace {

using json = nlohmann::json;
using UString = std::basic_string<UChar>;

// The pattern the ByteLevel pre-tokenizer splits with when use_regex is set.
constexpr const char* kByteLevelPattern =
    R"('s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+)";

std::string read_file(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path);
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

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

// tokenizers matches \s and \S against Unicode White_Space; ICU's \s omits a
// few of its characters (U+000B, U+0085), so they are spelled out.
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

// Decodes one code point of valid UTF-8 starting at text[i] and advances i.
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

bool flag(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

[[noreturn]] void unsupported(const std::string& path, const std::string& what) {
    throw std::runtime_error(path + " uses " + what + ", which amica-engine's tokenizer does not support");
}

}  // namespace

struct Tokenizer::Regex {
    URegularExpression* pattern = nullptr;
    ~Regex() {
        if (pattern) uregex_close(pattern);
    }
};

Tokenizer::Tokenizer(const std::string& path) : regex_(std::make_unique<Regex>()) {
    const json file = json::parse(read_file(path));

    const json& model = file.at("model");
    if (model.value("type", "") != "BPE") unsupported(path, "a model other than BPE");
    if (flag(model, "byte_fallback")) unsupported(path, "byte_fallback");
    if (model.contains("dropout") && !model.at("dropout").is_null()) unsupported(path, "BPE dropout");
    for (const char* affix : {"continuing_subword_prefix", "end_of_word_suffix"}) {
        if (model.contains(affix) && model.at(affix).is_string() && !model.at(affix).get<std::string>().empty()) {
            unsupported(path, affix);
        }
    }
    ignore_merges_ = flag(model, "ignore_merges");

    for (const auto& [token, id] : model.at("vocab").items()) {
        vocab_.emplace(token, id.get<std::int64_t>());
    }
    std::int64_t max_id = -1;
    for (const auto& [token, id] : vocab_) max_id = std::max(max_id, id);
    for (const auto& added : file.value("added_tokens", json::array())) {
        max_id = std::max(max_id, added.at("id").get<std::int64_t>());
    }
    id_to_token_.resize(static_cast<std::size_t>(max_id + 1));
    for (const auto& [token, id] : vocab_) id_to_token_[static_cast<std::size_t>(id)] = token;

    const json& merges = model.at("merges");
    for (std::size_t rank = 0; rank < merges.size(); ++rank) {
        std::string left;
        std::string right;
        if (merges[rank].is_array()) {
            left = merges[rank].at(0).get<std::string>();
            right = merges[rank].at(1).get<std::string>();
        } else {
            const std::string merge = merges[rank].get<std::string>();
            const std::size_t space = merge.find(' ');
            if (space == std::string::npos) unsupported(path, "a merge without two parts");
            left = merge.substr(0, space);
            right = merge.substr(space + 1);
        }
        const auto a = vocab_.find(left);
        const auto b = vocab_.find(right);
        const auto merged = vocab_.find(left + right);
        if (a == vocab_.end() || b == vocab_.end() || merged == vocab_.end()) {
            throw std::runtime_error(path + ": merge '" + left + " " + right + "' is not in the vocabulary");
        }
        const std::uint64_t key = (static_cast<std::uint64_t>(a->second) << 32) | static_cast<std::uint32_t>(b->second);
        merges_.emplace(key, Merge{static_cast<std::uint32_t>(rank), merged->second});
    }

    for (const auto& added : file.value("added_tokens", json::array())) {
        for (const char* option : {"single_word", "lstrip", "rstrip", "normalized"}) {
            if (flag(added, option)) unsupported(path, std::string("an added token with ") + option);
        }
        const std::string content = added.at("content").get<std::string>();
        const std::int64_t id = added.at("id").get<std::int64_t>();
        added_.push_back({content, id, flag(added, "special")});
        id_to_token_[static_cast<std::size_t>(id)] = content;
    }

    const json normalizer = file.value("normalizer", json());
    if (!normalizer.is_null()) {
        if (normalizer.value("type", "") != "NFC") unsupported(path, "a normalizer other than NFC");
        nfc_ = true;
    }

    const json pre = file.value("pre_tokenizer", json());
    const auto plain_byte_level = [](const json& step) {
        return step.value("type", "") == "ByteLevel" && !flag(step, "add_prefix_space");
    };
    std::string pattern;
    bool literal = false;
    if (pre.value("type", "") == "Sequence" && pre.at("pretokenizers").size() == 2 &&
        pre.at("pretokenizers")[0].value("type", "") == "Split" && plain_byte_level(pre.at("pretokenizers")[1]) &&
        !pre.at("pretokenizers")[1].value("use_regex", true)) {
        const json& split = pre.at("pretokenizers")[0];
        if (split.value("behavior", "") != "Isolated" || flag(split, "invert")) {
            unsupported(path, "a Split pre-tokenizer other than Isolated");
        }
        if (split.at("pattern").contains("Regex")) {
            pattern = to_icu_pattern(split.at("pattern").at("Regex").get<std::string>());
        } else {
            pattern = split.at("pattern").at("String").get<std::string>();
            literal = true;
        }
    } else if (plain_byte_level(pre) && pre.value("use_regex", true)) {
        pattern = to_icu_pattern(kByteLevelPattern);
    } else {
        unsupported(path, "this pre_tokenizer");
    }
    const json decoder = file.value("decoder", json());
    if (decoder.value("type", "") != "ByteLevel") unsupported(path, "a decoder other than ByteLevel");

    const UString pattern16 = to_utf16(pattern);
    UParseError parse_error{};
    UErrorCode status = U_ZERO_ERROR;
    regex_->pattern = uregex_open(pattern16.data(), static_cast<std::int32_t>(pattern16.size()),
                                  literal ? UREGEX_LITERAL : 0, &parse_error, &status);
    check_icu(status, "compiling the pre-tokenizer pattern");

    // GPT-2's reversible byte-to-character table: printable bytes keep their
    // own code point, the rest are moved to U+0100 onward.
    std::uint32_t shifted = 256;
    for (std::uint32_t b = 0; b < 256; ++b) {
        const bool printable = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255);
        const std::uint32_t code_point = printable ? b : shifted++;
        append_utf8(byte_to_char_[b], code_point);
        char_to_byte_[code_point] = static_cast<std::uint8_t>(b);
    }
}

Tokenizer::~Tokenizer() = default;

std::optional<std::int64_t> Tokenizer::token_to_id(const std::string& token) const {
    for (const auto& added : added_) {
        if (added.content == token) return added.id;
    }
    const auto it = vocab_.find(token);
    if (it == vocab_.end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> Tokenizer::special_tokens() const {
    std::vector<std::string> result;
    for (const auto& added : added_) {
        if (added.special) result.push_back(added.content);
    }
    return result;
}

std::vector<std::int64_t> Tokenizer::encode(const std::string& text) const {
    std::vector<std::int64_t> ids;
    std::size_t segment = 0;
    std::size_t i = 0;
    // Added tokens are matched leftmost-longest on the raw text, before normalization.
    while (i < text.size()) {
        const AddedToken* best = nullptr;
        for (const auto& added : added_) {
            if ((!best || added.content.size() > best->content.size()) &&
                text.compare(i, added.content.size(), added.content) == 0) {
                best = &added;
            }
        }
        if (!best) {
            ++i;
            continue;
        }
        encode_segment(text.substr(segment, i - segment), ids);
        ids.push_back(best->id);
        i += best->content.size();
        segment = i;
    }
    encode_segment(text.substr(segment), ids);
    return ids;
}

void Tokenizer::encode_segment(const std::string& segment, std::vector<std::int64_t>& ids) const {
    if (segment.empty()) return;
    const UString text = to_utf16(nfc_ ? nfc(segment) : segment);

    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<URegularExpression, decltype(&uregex_close)> regex(uregex_clone(regex_->pattern, &status),
                                                                       &uregex_close);
    check_icu(status, "uregex_clone");
    uregex_setText(regex.get(), text.data(), static_cast<std::int32_t>(text.size()), &status);
    check_icu(status, "uregex_setText");

    // Isolated split: every match is a word, and so is any text between matches.
    std::int32_t position = 0;
    const auto word = [&](std::int32_t begin, std::int32_t end) {
        if (end > begin) encode_word(to_utf8(text.data() + begin, end - begin), ids);
    };
    while (uregex_findNext(regex.get(), &status)) {
        const std::int32_t start = uregex_start(regex.get(), 0, &status);
        const std::int32_t end = uregex_end(regex.get(), 0, &status);
        check_icu(status, "uregex_findNext");
        if (end == start) continue;
        word(position, start);
        word(start, end);
        position = end;
    }
    check_icu(status, "uregex_findNext");
    word(position, static_cast<std::int32_t>(text.size()));
}

void Tokenizer::encode_word(const std::string& word, std::vector<std::int64_t>& ids) const {
    std::string mapped;
    std::vector<std::int64_t> parts;
    for (const char c : word) {
        const std::string& symbol = byte_to_char_[static_cast<std::uint8_t>(c)];
        mapped += symbol;
        const auto it = vocab_.find(symbol);
        if (it == vocab_.end()) {
            throw std::runtime_error("the tokenizer vocabulary has no entry for byte symbol '" + symbol + "'");
        }
        parts.push_back(it->second);
    }
    if (ignore_merges_) {
        const auto it = vocab_.find(mapped);
        if (it != vocab_.end()) {
            ids.push_back(it->second);
            return;
        }
    }
    // Repeatedly merge the adjacent pair with the lowest rank, leftmost first.
    while (parts.size() > 1) {
        std::uint32_t best_rank = std::numeric_limits<std::uint32_t>::max();
        std::size_t best = 0;
        std::int64_t best_id = 0;
        for (std::size_t k = 0; k + 1 < parts.size(); ++k) {
            const std::uint64_t key =
                (static_cast<std::uint64_t>(parts[k]) << 32) | static_cast<std::uint32_t>(parts[k + 1]);
            const auto it = merges_.find(key);
            if (it != merges_.end() && it->second.rank < best_rank) {
                best_rank = it->second.rank;
                best = k;
                best_id = it->second.id;
            }
        }
        if (best_rank == std::numeric_limits<std::uint32_t>::max()) break;
        parts[best] = best_id;
        parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(best) + 1);
    }
    ids.insert(ids.end(), parts.begin(), parts.end());
}

std::string Tokenizer::decode(const std::vector<std::int64_t>& ids) const {
    std::string bytes;
    for (const std::int64_t id : ids) {
        if (id < 0 || static_cast<std::size_t>(id) >= id_to_token_.size()) continue;
        const std::string& token = id_to_token_[static_cast<std::size_t>(id)];
        // A token with a character outside the byte table is kept as written,
        // as the ByteLevel decoder does.
        std::string token_bytes;
        bool mapped = true;
        for (std::size_t i = 0; i < token.size();) {
            const auto it = char_to_byte_.find(next_code_point(token, i));
            if (it == char_to_byte_.end()) {
                mapped = false;
                break;
            }
            token_bytes += static_cast<char>(it->second);
        }
        bytes += mapped ? token_bytes : token;
    }
    return utf8_lossy(bytes);
}

std::string utf8_lossy(const std::string& bytes) {
    static const std::string kReplacement = "\xEF\xBF\xBD";
    std::string result;
    result.reserve(bytes.size());
    const auto at = [&](std::size_t k) { return static_cast<std::uint8_t>(bytes[k]); };
    std::size_t i = 0;
    while (i < bytes.size()) {
        const std::uint8_t lead = at(i);
        if (lead < 0x80) {
            result += static_cast<char>(lead);
            ++i;
            continue;
        }
        int length = 0;
        std::uint8_t low = 0x80;
        std::uint8_t high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        }
        if (length == 0) {
            result += kReplacement;
            ++i;
            continue;
        }
        // Only the second byte has a lead-specific range; later ones are 80..BF.
        std::size_t k = 1;
        while (k < static_cast<std::size_t>(length) && i + k < bytes.size()) {
            const std::uint8_t b = at(i + k);
            const bool valid = k == 1 ? (b >= low && b <= high) : (b >= 0x80 && b <= 0xBF);
            if (!valid) break;
            ++k;
        }
        if (k == static_cast<std::size_t>(length)) {
            result.append(bytes, i, k);
        } else {
            result += kReplacement;
        }
        i += k;
    }
    return result;
}

}  // namespace amica_engine
