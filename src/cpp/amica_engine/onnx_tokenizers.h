#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace amica_engine {

class Regex;

// onnxruntime-extensions' CLIPTokenizer, read from the vocabulary and merges an
// ONNX tokenizer model carries. Whitespace runs collapse to their first
// character, line breaks are dropped, the text is lowercased and split with
// GPT-2's pattern, and each word is byte-level BPE encoded with "</w>" on its
// last byte.
class ClipTokenizer {
public:
    explicit ClipTokenizer(const std::string& onnx_path);
    ~ClipTokenizer();
    ClipTokenizer(const ClipTokenizer&) = delete;
    ClipTokenizer& operator=(const ClipTokenizer&) = delete;

    // Framed by the start and end tokens; the special tokens in the text are recognized.
    std::vector<std::int64_t> encode(const std::string& text) const;

    std::int64_t end_id() const { return end_id_; }

private:
    void encode_word(const std::string& word, std::vector<std::int64_t>& ids) const;
    std::int64_t id_of(const std::string& token) const;

    std::unordered_map<std::string, std::int64_t> vocab_;
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::int64_t>> merges_;  // rank, merged id
    std::int64_t byte_ids_[256] = {};
    std::int64_t end_of_word_byte_ids_[256] = {};
    std::int64_t start_id_ = 0;
    std::int64_t end_id_ = 0;
    std::unique_ptr<Regex> words_;
};

// Likewise for SentencepieceTokenizer with a Unigram SentencePiece proto: its
// precompiled normalization rules, then the best-scoring segmentation, with
// the BOS and EOS the graph's inputs ask for.
class SentencePieceTokenizer {
public:
    explicit SentencePieceTokenizer(const std::string& onnx_path);

    std::vector<std::int64_t> encode(const std::string& text) const;

    std::int64_t eos_id() const { return eos_id_; }
    std::int64_t pad_id() const { return pad_id_; }

private:
    std::string normalize(std::string_view text) const;
    std::pair<std::string_view, std::size_t> normalize_prefix(std::string_view input) const;

    std::unordered_map<std::string, std::int64_t> pieces_;
    std::vector<float> scores_;
    std::size_t max_piece_bytes_ = 0;
    float min_score_ = 0.0f;
    std::int64_t unk_id_ = 0;
    std::int64_t bos_id_ = -1;
    std::int64_t eos_id_ = -1;
    std::int64_t pad_id_ = -1;
    bool add_bos_ = false;
    bool add_eos_ = false;
    bool add_dummy_prefix_ = true;
    bool remove_extra_whitespaces_ = true;
    bool escape_whitespaces_ = true;
    std::vector<std::uint32_t> trie_;  // darts-clone double array over the rules' source text
    std::string replacements_;         // NUL-terminated replacement texts the trie's values index
};

}  // namespace amica_engine
