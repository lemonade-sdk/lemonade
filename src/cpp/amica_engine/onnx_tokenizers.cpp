#include "onnx_tokenizers.h"

#include "graph_abi.h"
#include "unicode.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>

namespace amica_engine {

namespace {

using json = nlohmann::json;

// One field of a protobuf message. Fixed-width values are kept in `value`.
struct ProtoField {
    std::uint32_t number = 0;
    std::uint32_t wire_type = 0;
    std::uint64_t value = 0;
    std::string_view bytes;  // a length-delimited field's payload
};

class ProtoReader {
public:
    explicit ProtoReader(std::string_view data) : data_(data) {}

    bool next(ProtoField& field) {
        if (at_ >= data_.size()) return false;
        const std::uint64_t key = varint();
        field.number = static_cast<std::uint32_t>(key >> 3);
        field.wire_type = static_cast<std::uint32_t>(key & 7);
        field.bytes = {};
        switch (field.wire_type) {
        case 0:
            field.value = varint();
            break;
        case 1:
            field.value = fixed(8);
            break;
        case 2: {
            const std::uint64_t size = varint();
            if (size > data_.size() - at_) throw std::runtime_error("truncated protobuf field");
            field.bytes = data_.substr(at_, static_cast<std::size_t>(size));
            at_ += static_cast<std::size_t>(size);
            break;
        }
        case 5:
            field.value = fixed(4);
            break;
        default:
            throw std::runtime_error("unsupported protobuf wire type " + std::to_string(field.wire_type));
        }
        return true;
    }

private:
    std::uint64_t varint() {
        std::uint64_t result = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (at_ >= data_.size()) throw std::runtime_error("truncated protobuf varint");
            const auto byte = static_cast<std::uint8_t>(data_[at_++]);
            result |= std::uint64_t{byte & 0x7Fu} << shift;
            if ((byte & 0x80) == 0) return result;
        }
        throw std::runtime_error("malformed protobuf varint");
    }

    std::uint64_t fixed(std::size_t size) {
        if (size > data_.size() - at_) throw std::runtime_error("truncated protobuf field");
        std::uint64_t result = 0;
        std::memcpy(&result, data_.data() + at_, size);
        at_ += size;
        return result;
    }

    std::string_view data_;
    std::size_t at_ = 0;
};

float as_float(const ProtoField& field) {
    const auto bits = static_cast<std::uint32_t>(field.value);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct TokenizerNode {
    std::map<std::string, std::string> attributes;
    std::vector<std::string> inputs;
    std::map<std::string, bool> booleans;

    bool input_flag(std::size_t index) const {
        if (index >= inputs.size()) return false;
        const auto it = booleans.find(inputs[index]);
        return it != booleans.end() && it->second;
    }
};

bool tensor_flag(std::string_view tensor) {
    ProtoReader reader(tensor);
    ProtoField field;
    while (reader.next(field)) {
        if (field.number == 9 && !field.bytes.empty()) return field.bytes[0] != 0;  // raw_data
        if (field.number == 5) {                                                    // int32_data
            // Packed varints: the first byte is zero only for a zero value.
            return field.wire_type == 0 ? field.value != 0 : !field.bytes.empty() && field.bytes[0] != 0;
        }
    }
    return false;
}

std::string tensor_name(std::string_view tensor) {
    ProtoReader reader(tensor);
    ProtoField field;
    while (reader.next(field)) {
        if (field.number == 8) return std::string(field.bytes);
    }
    return {};
}

TokenizerNode read_tokenizer_node(const std::string& path, const std::string& op_type) {
    const std::string model = read_text_file(path);
    std::string_view graph;
    ProtoReader reader(model);
    ProtoField field;
    while (reader.next(field)) {
        if (field.number == 7) graph = field.bytes;
    }
    TokenizerNode result;
    bool found = false;
    std::map<std::string, bool> initializers;
    ProtoReader graph_reader(graph);
    while (graph_reader.next(field)) {
        if (field.number == 5) {
            initializers[tensor_name(field.bytes)] = tensor_flag(field.bytes);
            continue;
        }
        if (field.number != 1) continue;
        TokenizerNode node;
        std::string type;
        ProtoReader node_reader(field.bytes);
        ProtoField node_field;
        while (node_reader.next(node_field)) {
            if (node_field.number == 1) {
                node.inputs.emplace_back(node_field.bytes);
            } else if (node_field.number == 4) {
                type = std::string(node_field.bytes);
            } else if (node_field.number == 5) {
                std::string name;
                std::string value;
                ProtoReader attribute(node_field.bytes);
                ProtoField attribute_field;
                while (attribute.next(attribute_field)) {
                    if (attribute_field.number == 1) name = std::string(attribute_field.bytes);
                    if (attribute_field.number == 4) value = std::string(attribute_field.bytes);
                }
                node.attributes[name] = value;
            }
        }
        if (type == op_type) {
            if (found) throw std::runtime_error(path + " has more than one " + op_type + " node");
            result = std::move(node);
            found = true;
        }
    }
    if (!found) throw std::runtime_error(path + " has no " + op_type + " node");
    result.booleans = std::move(initializers);
    return result;
}

const std::string& attribute(const TokenizerNode& node, const std::string& name, const std::string& path) {
    const auto it = node.attributes.find(name);
    if (it == node.attributes.end() || it->second.empty()) {
        throw std::runtime_error(path + ": the tokenizer node has no '" + name + "' attribute");
    }
    return it->second;
}

const char* const kStartOfText = "<|startoftext|>";
const char* const kEndOfText = "<|endoftext|>";

std::size_t utf8_length(std::uint8_t lead) {
    return lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
}

}  // namespace

ClipTokenizer::ClipTokenizer(const std::string& onnx_path) {
    const TokenizerNode node = read_tokenizer_node(onnx_path, "CLIPTokenizer");
    const json vocab = json::parse(attribute(node, "vocab", onnx_path));
    for (const auto& [token, id] : vocab.items()) {
        vocab_.emplace(token, id.get<std::int64_t>());
    }
    start_id_ = id_of(kStartOfText);
    end_id_ = id_of(kEndOfText);

    // GPT-2's reversible byte-to-character table: printable bytes keep their
    // own code point, the rest are moved to U+0100 onward.
    std::uint32_t shifted = 256;
    for (std::uint32_t b = 0; b < 256; ++b) {
        const bool printable = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255);
        std::string symbol;
        append_utf8(symbol, printable ? b : shifted++);
        byte_ids_[b] = id_of(symbol);
        end_of_word_byte_ids_[b] = id_of(symbol + "</w>");
    }

    const std::string& merges = attribute(node, "merges", onnx_path);
    std::uint32_t rank = 0;
    for (std::size_t begin = 0; begin < merges.size();) {
        std::size_t end = merges.find('\n', begin);
        if (end == std::string::npos) end = merges.size();
        std::string line = merges.substr(begin, end - begin);
        begin = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("#version", 0) == 0) continue;
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) throw std::runtime_error(onnx_path + ": merge '" + line + "' has one part");
        const std::string left = line.substr(0, space);
        const std::string right = line.substr(space + 1);
        const std::uint64_t key = (static_cast<std::uint64_t>(id_of(left)) << 32) |
                                  static_cast<std::uint32_t>(id_of(right));
        merges_.emplace(key, std::make_pair(rank++, id_of(left + right)));
    }
    words_ = std::make_unique<Regex>(to_icu_pattern(kByteLevelPattern), false);
}

ClipTokenizer::~ClipTokenizer() = default;

std::int64_t ClipTokenizer::id_of(const std::string& token) const {
    const auto it = vocab_.find(token);
    if (it == vocab_.end()) throw std::runtime_error("CLIP token '" + token + "' is not in the vocabulary");
    return it->second;
}

std::vector<std::int64_t> ClipTokenizer::encode(const std::string& text) const {
    std::string cleaned;
    bool in_whitespace = false;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t begin = i;
        const bool whitespace = is_whitespace(next_code_point(text, i));
        if (!(whitespace && in_whitespace)) cleaned.append(text, begin, i - begin);
        in_whitespace = whitespace;
    }
    const auto edge_whitespace_length = [&](bool at_end) -> std::size_t {
        if (cleaned.empty()) return 0;
        std::size_t begin = 0;
        if (at_end) {
            begin = cleaned.size() - 1;
            while (begin > 0 && (static_cast<std::uint8_t>(cleaned[begin]) & 0xC0) == 0x80) --begin;
        }
        std::size_t at = begin;
        return is_whitespace(next_code_point(cleaned, at)) ? at - begin : 0;
    };
    cleaned.erase(0, edge_whitespace_length(false));
    cleaned.erase(cleaned.size() - edge_whitespace_length(true));
    cleaned.erase(std::remove_if(cleaned.begin(), cleaned.end(), [](char c) { return c == '\n' || c == '\r'; }),
                  cleaned.end());
    cleaned = lowercase(cleaned);

    std::vector<std::int64_t> ids{start_id_};
    const auto encode_segment = [&](const std::string& segment) {
        if (segment.empty()) return;
        for (auto [word, matched] : words_->split(segment)) {
            word.erase(std::remove(word.begin(), word.end(), ' '), word.end());
            if (!word.empty()) encode_word(word, ids);
        }
    };
    std::size_t segment = 0;
    for (std::size_t at = 0; at < cleaned.size();) {
        std::int64_t special = -1;
        std::size_t length = 0;
        for (const char* token : {kStartOfText, kEndOfText}) {
            if (cleaned.compare(at, std::strlen(token), token) == 0) {
                special = id_of(token);
                length = std::strlen(token);
            }
        }
        if (special < 0) {
            ++at;
            continue;
        }
        encode_segment(cleaned.substr(segment, at - segment));
        ids.push_back(special);
        at += length;
        segment = at;
    }
    encode_segment(cleaned.substr(segment));
    ids.push_back(end_id_);
    return ids;
}

void ClipTokenizer::encode_word(const std::string& word, std::vector<std::int64_t>& ids) const {
    std::vector<std::int64_t> parts;
    for (std::size_t i = 0; i < word.size(); ++i) {
        const auto byte = static_cast<std::uint8_t>(word[i]);
        parts.push_back(i + 1 == word.size() ? end_of_word_byte_ids_[byte] : byte_ids_[byte]);
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
            if (it != merges_.end() && it->second.first < best_rank) {
                best_rank = it->second.first;
                best = k;
                best_id = it->second.second;
            }
        }
        if (best_rank == std::numeric_limits<std::uint32_t>::max()) break;
        parts[best] = best_id;
        parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(best) + 1);
    }
    ids.insert(ids.end(), parts.begin(), parts.end());
}

SentencePieceTokenizer::SentencePieceTokenizer(const std::string& onnx_path) {
    const TokenizerNode node = read_tokenizer_node(onnx_path, "SentencepieceTokenizer");
    // Inputs: text, nbest_size, alpha, add_bos, add_eos, reverse, fairseq.
    add_bos_ = node.input_flag(3);
    add_eos_ = node.input_flag(4);
    if (node.input_flag(5) || node.input_flag(6)) {
        throw std::runtime_error(onnx_path + ": reversed or fairseq SentencePiece output is not supported");
    }

    const auto unsupported = [&](const std::string& what) {
        throw std::runtime_error(onnx_path + ": the SentencePiece model uses " + what +
                                 ", which amica-engine does not support");
    };
    std::vector<std::pair<std::string, std::uint64_t>> pieces;  // text, type
    std::string_view charsmap;
    ProtoReader model(attribute(node, "model", onnx_path));
    ProtoField field;
    while (model.next(field)) {
        if (field.number == 1) {
            std::string piece;
            float score = 0.0f;
            std::uint64_t type = 1;
            ProtoReader reader(field.bytes);
            ProtoField piece_field;
            while (reader.next(piece_field)) {
                if (piece_field.number == 1) piece = std::string(piece_field.bytes);
                if (piece_field.number == 2) score = as_float(piece_field);
                if (piece_field.number == 3) type = piece_field.value;
            }
            pieces.emplace_back(piece, type);
            scores_.push_back(score);
        } else if (field.number == 2) {
            ProtoReader reader(field.bytes);
            ProtoField spec;
            while (reader.next(spec)) {
                const auto id = static_cast<std::int64_t>(static_cast<std::int32_t>(spec.value));
                switch (spec.number) {
                case 3:
                    if (spec.value != 1) unsupported("a model type other than Unigram");
                    break;
                case 24:
                    if (spec.value != 0) unsupported("treat_whitespace_as_suffix");
                    break;
                case 35:
                    if (spec.value != 0) unsupported("byte fallback");
                    break;
                case 40: unk_id_ = id; break;
                case 41: bos_id_ = id; break;
                case 42: eos_id_ = id; break;
                case 43: pad_id_ = id; break;
                default: break;
                }
            }
        } else if (field.number == 3) {
            ProtoReader reader(field.bytes);
            ProtoField spec;
            while (reader.next(spec)) {
                if (spec.number == 2) charsmap = spec.bytes;
                if (spec.number == 3) add_dummy_prefix_ = spec.value != 0;
                if (spec.number == 4) remove_extra_whitespaces_ = spec.value != 0;
                if (spec.number == 5) escape_whitespaces_ = spec.value != 0;
            }
        }
    }
    if (pieces.empty()) throw std::runtime_error(onnx_path + ": the SentencePiece model has no pieces");

    min_score_ = std::numeric_limits<float>::max();
    for (std::size_t id = 0; id < pieces.size(); ++id) {
        const auto& [piece, type] = pieces[id];
        if (type == 4) unsupported("user-defined pieces");
        if (type == 6) unsupported("byte pieces");
        if (type == 1) {
            pieces_.emplace(piece, static_cast<std::int64_t>(id));
            max_piece_bytes_ = std::max(max_piece_bytes_, piece.size());
            min_score_ = std::min(min_score_, scores_[id]);
        }
    }
    if (unk_id_ < 0 || static_cast<std::size_t>(unk_id_) >= pieces.size()) {
        throw std::runtime_error(onnx_path + ": the SentencePiece model has no unknown piece");
    }

    // The precompiled rules: the trie's size, the trie, then the replacements.
    if (!charsmap.empty()) {
        std::uint32_t trie_size = 0;
        if (charsmap.size() < sizeof(trie_size)) throw std::runtime_error(onnx_path + ": truncated charsmap");
        std::memcpy(&trie_size, charsmap.data(), sizeof(trie_size));
        if (trie_size >= charsmap.size() - sizeof(trie_size) || trie_size % sizeof(std::uint32_t) != 0) {
            throw std::runtime_error(onnx_path + ": malformed charsmap");
        }
        trie_.resize(trie_size / sizeof(std::uint32_t));
        std::memcpy(trie_.data(), charsmap.data() + sizeof(trie_size), trie_size);
        replacements_ = std::string(charsmap.substr(sizeof(trie_size) + trie_size));
    }
}

// The longest normalization rule matching the start of input, as
// SentencePiece's Normalizer::NormalizePrefix: its replacement and the bytes it
// consumes; one character unchanged when no rule matches.
std::pair<std::string_view, std::size_t> SentencePieceTokenizer::normalize_prefix(std::string_view input) const {
    std::size_t longest = 0;
    std::uint32_t value = 0;
    if (!trie_.empty()) {
        // darts-clone's commonPrefixSearch.
        const auto offset = [](std::uint32_t unit) { return (unit >> 10) << ((unit & (1u << 9)) >> 6); };
        std::size_t node = offset(trie_[0]);
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto byte = static_cast<std::uint8_t>(input[i]);
            node ^= byte;
            if (node >= trie_.size()) break;
            const std::uint32_t unit = trie_[node];
            if ((unit & ((1u << 31) | 0xFFu)) != byte) break;
            node ^= offset(unit);
            if (node >= trie_.size()) break;
            if (((unit >> 8) & 1u) != 0) {
                longest = i + 1;
                value = trie_[node] & ((1u << 31) - 1);
            }
        }
    }
    if (longest != 0) {
        if (value >= replacements_.size()) throw std::runtime_error("malformed SentencePiece charsmap");
        return {std::string_view(replacements_.c_str() + value), longest};
    }
    const std::size_t length = std::min(utf8_length(static_cast<std::uint8_t>(input[0])), input.size());
    return {input.substr(0, length), length};
}

std::string SentencePieceTokenizer::normalize(std::string_view input) const {
    static const std::string kSpace = "\xE2\x96\x81";  // U+2581, SentencePiece's visible space
    std::string normalized;
    if (remove_extra_whitespaces_) {
        while (!input.empty()) {
            const auto [piece, consumed] = normalize_prefix(input);
            if (piece != " ") break;
            input.remove_prefix(consumed);
        }
    }
    if (input.empty()) return normalized;
    if (add_dummy_prefix_) normalized += escape_whitespaces_ ? kSpace : " ";
    bool previous_space = remove_extra_whitespaces_;
    while (!input.empty()) {
        auto [piece, consumed] = normalize_prefix(input);
        while (previous_space && !piece.empty() && piece.front() == ' ') piece.remove_prefix(1);
        if (!piece.empty()) {
            for (const char c : piece) {
                if (escape_whitespaces_ && c == ' ') {
                    normalized += kSpace;
                } else {
                    normalized += c;
                }
            }
            previous_space = piece.back() == ' ';
        }
        input.remove_prefix(consumed);
    }
    if (remove_extra_whitespaces_) {
        const std::string space = escape_whitespaces_ ? kSpace : " ";
        while (normalized.size() >= space.size() &&
               normalized.compare(normalized.size() - space.size(), space.size(), space) == 0) {
            normalized.resize(normalized.size() - space.size());
        }
    }
    return normalized;
}

std::vector<std::int64_t> SentencePieceTokenizer::encode(const std::string& text) const {
    // SentencePiece's Unigram EncodeOptimized: the best-scoring path through
    // every vocabulary piece, where a character no piece starts with costs the
    // unknown piece's score of the lowest piece score minus 10.
    const std::string normalized = normalize(text);
    const std::size_t size = normalized.size();
    struct Node {
        std::int64_t id = -1;
        float score = 0.0f;
        std::ptrdiff_t starts_at = -1;
    };
    std::vector<Node> best(size + 1);
    const float unknown_score = min_score_ - 10.0f;
    for (std::size_t start = 0; start < size;) {
        const float score_here = best[start].score;
        const std::size_t char_length = std::min(utf8_length(static_cast<std::uint8_t>(normalized[start])),
                                                 size - start);
        bool single_char_piece = false;
        const std::size_t longest = std::min(max_piece_bytes_, size - start);
        for (std::size_t length = 1; length <= longest; ++length) {
            const auto it = pieces_.find(normalized.substr(start, length));
            if (it == pieces_.end()) continue;
            Node& target = best[start + length];
            const float candidate = scores_[static_cast<std::size_t>(it->second)] + score_here;
            if (target.starts_at < 0 || candidate > target.score) {
                target = {it->second, candidate, static_cast<std::ptrdiff_t>(start)};
            }
            if (length == char_length) single_char_piece = true;
        }
        if (!single_char_piece) {
            Node& target = best[start + char_length];
            const float candidate = unknown_score + score_here;
            if (target.starts_at < 0 || candidate > target.score) {
                target = {unk_id_, candidate, static_cast<std::ptrdiff_t>(start)};
            }
        }
        start += char_length;
    }
    std::vector<std::int64_t> ids;
    for (std::size_t end = size; end > 0; end = static_cast<std::size_t>(best[end].starts_at)) {
        // SentencePieceProcessor merges a run of unknown pieces into one.
        if (best[end].id == unk_id_ && !ids.empty() && ids.back() == unk_id_) continue;
        ids.push_back(best[end].id);
    }
    std::reverse(ids.begin(), ids.end());
    if (add_bos_ && bos_id_ >= 0) ids.insert(ids.begin(), bos_id_);
    if (add_eos_ && eos_id_ >= 0) ids.push_back(eos_id_);
    return ids;
}

}  // namespace amica_engine
