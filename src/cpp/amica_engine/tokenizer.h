#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace amica_engine {

// A byte-level BPE tokenizer read from a Hugging Face tokenizer.json. It
// reproduces the tokenizers library for the configurations ChatApp bundles use
// (NFC or no normalizer, a Split regex or ByteLevel pre-tokenizer, BPE, a
// ByteLevel decoder); a file using anything else is rejected when it is loaded
// rather than tokenized differently.
class Tokenizer {
public:
    explicit Tokenizer(const std::string& path);
    ~Tokenizer();
    Tokenizer(const Tokenizer&) = delete;
    Tokenizer& operator=(const Tokenizer&) = delete;

    // Added tokens in the text are recognized; no BOS or EOS is added.
    std::vector<std::int64_t> encode(const std::string& text) const;

    // Special tokens are kept. Bytes that do not form valid UTF-8 become
    // U+FFFD, one per maximal invalid subsequence.
    std::string decode(const std::vector<std::int64_t>& ids) const;

    std::optional<std::int64_t> token_to_id(const std::string& token) const;

    // Contents of the added tokens marked special.
    std::vector<std::string> special_tokens() const;

private:
    struct AddedToken {
        std::string content;
        std::int64_t id;
        bool special;
    };
    struct Merge {
        std::uint32_t rank;
        std::int64_t id;
    };

    void encode_segment(const std::string& text, std::vector<std::int64_t>& ids) const;
    void encode_word(const std::string& word, std::vector<std::int64_t>& ids) const;

    std::unordered_map<std::string, std::int64_t> vocab_;
    std::vector<std::string> id_to_token_;
    std::unordered_map<std::uint64_t, Merge> merges_;
    std::vector<AddedToken> added_;
    bool nfc_ = false;
    bool ignore_merges_ = false;
    std::string byte_to_char_[256];
    std::unordered_map<std::uint32_t, std::uint8_t> char_to_byte_;

    struct Regex;
    std::unique_ptr<Regex> regex_;
};

// Replaces each maximal invalid UTF-8 subsequence with U+FFFD, as Rust's
// String::from_utf8_lossy does.
std::string utf8_lossy(const std::string& bytes);

}  // namespace amica_engine
