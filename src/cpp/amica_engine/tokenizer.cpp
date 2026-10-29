#include "tokenizer.h"

#include "unicode.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace amica_engine {

namespace {

using json = nlohmann::json;

std::string read_file(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path);
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

bool flag(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

[[noreturn]] void unsupported(const std::string& path, const std::string& what) {
    throw std::runtime_error(path + " uses " + what + ", which amica-engine's tokenizer does not support");
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

// The length of the UTF-8 character ending at text[end - 1].
std::size_t previous_char_length(const std::string& text, std::size_t end) {
    std::size_t begin = end - 1;
    while (begin > 0 && (static_cast<std::uint8_t>(text[begin]) & 0xC0) == 0x80) --begin;
    return end - begin;
}

std::string string_pattern(const json& step, const std::string& path, const char* what) {
    const json& pattern = step.at("pattern");
    if (!pattern.contains("String")) unsupported(path, std::string("a regex ") + what);
    return pattern.at("String").get<std::string>();
}

// Expands a component and its nested Sequences into its steps, in order.
void flatten(const json& component, const char* list_key, std::vector<json>& steps) {
    if (component.is_null()) return;
    if (component.value("type", "") == "Sequence") {
        for (const auto& step : component.at(list_key)) flatten(step, list_key, steps);
    } else {
        steps.push_back(component);
    }
}

}  // namespace

Tokenizer::Tokenizer(const std::string& path) {
    const json file = json::parse(read_file(path));

    const json& model = file.at("model");
    if (model.value("type", "") != "BPE") unsupported(path, "a model other than BPE");
    if (model.contains("dropout") && !model.at("dropout").is_null()) unsupported(path, "BPE dropout");
    for (const char* affix : {"continuing_subword_prefix", "end_of_word_suffix"}) {
        if (model.contains(affix) && model.at(affix).is_string() && !model.at(affix).get<std::string>().empty()) {
            unsupported(path, affix);
        }
    }
    ignore_merges_ = flag(model, "ignore_merges");
    byte_fallback_ = flag(model, "byte_fallback");
    fuse_unk_ = flag(model, "fuse_unk");

    for (const auto& [token, id] : model.at("vocab").items()) {
        vocab_.emplace(token, id.get<std::int64_t>());
    }
    if (model.contains("unk_token") && model.at("unk_token").is_string()) {
        const auto it = vocab_.find(model.at("unk_token").get<std::string>());
        if (it != vocab_.end()) unk_id_ = it->second;
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
        for (const char* option : {"single_word", "normalized"}) {
            if (flag(added, option)) unsupported(path, std::string("an added token with ") + option);
        }
        const std::string content = added.at("content").get<std::string>();
        const std::int64_t id = added.at("id").get<std::int64_t>();
        added_.push_back({content, id, flag(added, "special"), flag(added, "lstrip"), flag(added, "rstrip")});
        id_to_token_[static_cast<std::size_t>(id)] = content;
    }

    std::vector<json> steps;
    flatten(file.value("normalizer", json()), "normalizers", steps);
    for (const auto& step : steps) {
        const std::string type = step.value("type", "");
        if (type == "NFC") {
            normalizers_.push_back({Normalizer::Kind::Nfc, "", ""});
        } else if (type == "Prepend") {
            normalizers_.push_back({Normalizer::Kind::Prepend, "", step.at("prepend").get<std::string>()});
        } else if (type == "Replace") {
            normalizers_.push_back({Normalizer::Kind::Replace, string_pattern(step, path, "Replace normalizer"),
                                    step.at("content").get<std::string>()});
        } else {
            unsupported(path, "the " + type + " normalizer");
        }
    }

    steps.clear();
    flatten(file.value("pre_tokenizer", json()), "pretokenizers", steps);
    for (const auto& step : steps) {
        const std::string type = step.value("type", "");
        if (type == "Split") {
            const std::string behavior = step.value("behavior", "");
            if (behavior != "Isolated" && behavior != "Removed") unsupported(path, "Split behavior " + behavior);
            PreTokenizer split{PreTokenizer::Kind::Split};
            const json& pattern = step.at("pattern");
            split.regex = pattern.contains("Regex")
                              ? std::make_shared<Regex>(to_icu_pattern(pattern.at("Regex").get<std::string>()), false)
                              : std::make_shared<Regex>(pattern.at("String").get<std::string>(), true);
            split.removed = behavior == "Removed";
            split.invert = flag(step, "invert");
            pre_tokenizers_.push_back(split);
        } else if (type == "ByteLevel") {
            if (flag(step, "add_prefix_space")) unsupported(path, "ByteLevel with add_prefix_space");
            PreTokenizer byte_level{PreTokenizer::Kind::ByteLevel};
            if (step.value("use_regex", true)) {
                byte_level.regex = std::make_shared<Regex>(to_icu_pattern(kByteLevelPattern), false);
            }
            pre_tokenizers_.push_back(byte_level);
        } else {
            unsupported(path, "the " + type + " pre-tokenizer");
        }
    }

    steps.clear();
    flatten(file.value("decoder", json()), "decoders", steps);
    for (const auto& step : steps) {
        const std::string type = step.value("type", "");
        if (type == "ByteLevel") {
            decoders_.push_back({Decoder::Kind::ByteLevel});
        } else if (type == "Replace") {
            decoders_.push_back({Decoder::Kind::Replace, string_pattern(step, path, "Replace decoder"),
                                 step.at("content").get<std::string>()});
        } else if (type == "ByteFallback") {
            decoders_.push_back({Decoder::Kind::ByteFallback});
        } else if (type == "Fuse") {
            decoders_.push_back({Decoder::Kind::Fuse});
        } else if (type == "Strip") {
            decoders_.push_back({Decoder::Kind::Strip, "", step.at("content").get<std::string>(),
                                 step.value("start", std::size_t{0}), step.value("stop", std::size_t{0})});
        } else {
            unsupported(path, "the " + type + " decoder");
        }
    }

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
    // Added tokens are matched leftmost-longest on the raw text, before
    // normalization. A token with lstrip/rstrip also takes the whitespace
    // beside it, though never text an earlier token already took.
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
        std::size_t start = i;
        std::size_t stop = i + best->content.size();
        if (best->lstrip) {
            while (start > segment) {
                const std::size_t length = previous_char_length(text, start);
                std::size_t at = start - length;
                if (!is_whitespace(next_code_point(text, at))) break;
                start -= length;
            }
        }
        if (best->rstrip) {
            while (stop < text.size()) {
                std::size_t at = stop;
                if (!is_whitespace(next_code_point(text, at))) break;
                stop = at;
            }
        }
        encode_segment(text.substr(segment, start - segment), ids);
        ids.push_back(best->id);
        i += best->content.size();
        segment = stop;
        i = std::max(i, segment);
    }
    encode_segment(text.substr(segment), ids);
    return ids;
}

void Tokenizer::encode_segment(const std::string& segment, std::vector<std::int64_t>& ids) const {
    if (segment.empty()) return;
    std::string normalized = segment;
    for (const auto& step : normalizers_) {
        switch (step.kind) {
        case Normalizer::Kind::Nfc:
            normalized = nfc(normalized);
            break;
        case Normalizer::Kind::Prepend:
            if (!normalized.empty()) normalized = step.to + normalized;
            break;
        case Normalizer::Kind::Replace:
            replace_all(normalized, step.from, step.to);
            break;
        }
    }

    std::vector<std::string> pieces{normalized};
    for (const auto& step : pre_tokenizers_) {
        std::vector<std::string> next;
        for (const auto& piece : pieces) {
            if (!step.regex) {
                next.push_back(piece);
                continue;
            }
            // Isolated keeps every match and every gap as its own piece;
            // Removed drops the matches. invert swaps which parts are matches.
            for (const auto& [part, matched] : step.regex->split(piece)) {
                if (step.kind == PreTokenizer::Kind::Split && step.removed && matched != step.invert) continue;
                next.push_back(part);
            }
        }
        if (step.kind == PreTokenizer::Kind::ByteLevel) {
            for (auto& piece : next) {
                std::string mapped;
                for (const char c : piece) mapped += byte_to_char_[static_cast<std::uint8_t>(c)];
                piece = mapped;
            }
        }
        pieces = std::move(next);
    }
    for (const auto& piece : pieces) {
        if (!piece.empty()) encode_word(piece, ids);
    }
}

void Tokenizer::encode_word(const std::string& word, std::vector<std::int64_t>& ids) const {
    if (ignore_merges_) {
        const auto it = vocab_.find(word);
        if (it != vocab_.end()) {
            ids.push_back(it->second);
            return;
        }
    }
    // One symbol per character. A character outside the vocabulary becomes
    // its <0xNN> byte tokens with byte fallback, otherwise the unknown token.
    std::vector<std::int64_t> parts;
    bool previous_unknown = false;
    for (std::size_t i = 0; i < word.size();) {
        const std::size_t begin = i;
        next_code_point(word, i);
        const std::string symbol = word.substr(begin, i - begin);
        const auto it = vocab_.find(symbol);
        if (it != vocab_.end()) {
            parts.push_back(it->second);
            previous_unknown = false;
            continue;
        }
        if (byte_fallback_) {
            std::vector<std::int64_t> bytes;
            for (const char c : symbol) {
                char name[8];
                std::snprintf(name, sizeof(name), "<0x%02X>", static_cast<unsigned>(static_cast<std::uint8_t>(c)));
                const auto byte = vocab_.find(name);
                if (byte == vocab_.end()) break;
                bytes.push_back(byte->second);
            }
            if (bytes.size() == symbol.size()) {
                parts.insert(parts.end(), bytes.begin(), bytes.end());
                previous_unknown = false;
                continue;
            }
        }
        if (unk_id_) {
            if (!(fuse_unk_ && previous_unknown)) parts.push_back(*unk_id_);
            previous_unknown = true;
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
    std::vector<std::string> tokens;
    for (const std::int64_t id : ids) {
        if (id >= 0 && static_cast<std::size_t>(id) < id_to_token_.size()) {
            tokens.push_back(id_to_token_[static_cast<std::size_t>(id)]);
        }
    }
    for (const auto& step : decoders_) {
        switch (step.kind) {
        case Decoder::Kind::ByteLevel: {
            // A token with a character outside the byte table is kept as written.
            std::string bytes;
            for (const auto& token : tokens) {
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
            tokens = {utf8_lossy(bytes)};
            break;
        }
        case Decoder::Kind::Replace:
            for (auto& token : tokens) replace_all(token, step.from, step.to);
            break;
        case Decoder::Kind::ByteFallback: {
            // A run of <0xNN> tokens becomes the text its bytes spell, or one
            // U+FFFD per byte when they are not valid UTF-8.
            std::vector<std::string> result;
            std::string bytes;
            std::size_t byte_tokens = 0;
            const auto flush = [&]() {
                if (byte_tokens == 0) return;
                if (utf8_lossy(bytes) == bytes) {
                    result.push_back(bytes);
                } else {
                    for (std::size_t k = 0; k < byte_tokens; ++k) result.push_back("\xEF\xBF\xBD");
                }
                bytes.clear();
                byte_tokens = 0;
            };
            for (const auto& token : tokens) {
                const bool byte_token = token.size() == 6 && token.compare(0, 3, "<0x") == 0 && token[5] == '>' &&
                                        std::isxdigit(static_cast<unsigned char>(token[3])) &&
                                        std::isxdigit(static_cast<unsigned char>(token[4]));
                if (byte_token) {
                    bytes += static_cast<char>(std::stoi(token.substr(3, 2), nullptr, 16));
                    ++byte_tokens;
                } else {
                    flush();
                    result.push_back(token);
                }
            }
            flush();
            tokens = std::move(result);
            break;
        }
        case Decoder::Kind::Fuse: {
            std::string fused;
            for (const auto& token : tokens) fused += token;
            tokens = {fused};
            break;
        }
        case Decoder::Kind::Strip:
            for (auto& token : tokens) {
                for (std::size_t k = 0; k < step.start && token.compare(0, step.to.size(), step.to) == 0; ++k) {
                    token.erase(0, step.to.size());
                }
                for (std::size_t k = 0; k < step.stop && token.size() >= step.to.size() &&
                                        token.compare(token.size() - step.to.size(), step.to.size(), step.to) == 0;
                     ++k) {
                    token.erase(token.size() - step.to.size());
                }
            }
            break;
        }
    }
    std::string text;
    for (const auto& token : tokens) text += token;
    return utf8_lossy(text);
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
