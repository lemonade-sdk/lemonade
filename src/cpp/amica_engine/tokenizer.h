#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace amica_engine {

class Regex;

// A BPE tokenizer read from a Hugging Face tokenizer.json, reproducing the
// tokenizers library for the components ChatApp bundles use: NFC, Prepend and
// Replace normalizers; Split and ByteLevel pre-tokenizers; BPE with byte
// fallback and an unknown token; ByteLevel, Replace, ByteFallback, Fuse and
// Strip decoders; added tokens that strip surrounding whitespace. A file using
// anything else is rejected when it is loaded rather than tokenized differently.
class Tokenizer {
public:
    explicit Tokenizer(const std::string& path);
    ~Tokenizer();
    Tokenizer(const Tokenizer&) = delete;
    Tokenizer& operator=(const Tokenizer&) = delete;

    // Added tokens in the text are recognized; no BOS or EOS is added.
    std::vector<std::int64_t> encode(const std::string& text) const;

    // Special tokens are kept. Bytes that do not form valid UTF-8 become U+FFFD.
    std::string decode(const std::vector<std::int64_t>& ids) const;

    std::optional<std::int64_t> token_to_id(const std::string& token) const;

    // Contents of the added tokens marked special.
    std::vector<std::string> special_tokens() const;

    struct Normalizer {
        enum class Kind { Nfc, Prepend, Replace } kind;
        std::string from;  // Replace pattern
        std::string to;    // Prepend text or Replace content
    };
    struct PreTokenizer {
        enum class Kind { Split, ByteLevel } kind;
        std::shared_ptr<Regex> regex;  // Split pattern, or ByteLevel's own when it uses one
        bool removed = false;          // Split behavior Removed rather than Isolated
        bool invert = false;
    };
    struct Decoder {
        enum class Kind { ByteLevel, Replace, ByteFallback, Fuse, Strip } kind;
        std::string from;
        std::string to;  // Replace content, or the Strip character
        std::size_t start = 0;
        std::size_t stop = 0;
    };

private:
    struct AddedToken {
        std::string content;
        std::int64_t id;
        bool special;
        bool lstrip;
        bool rstrip;
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
    std::vector<Normalizer> normalizers_;
    std::vector<PreTokenizer> pre_tokenizers_;
    std::vector<Decoder> decoders_;
    bool ignore_merges_ = false;
    bool byte_fallback_ = false;
    bool fuse_unk_ = false;
    std::optional<std::int64_t> unk_id_;
    std::string byte_to_char_[256];
    std::unordered_map<std::uint32_t, std::uint8_t> char_to_byte_;
};

// Replaces each maximal invalid UTF-8 subsequence with U+FFFD, as Rust's
// String::from_utf8_lossy does.
std::string utf8_lossy(const std::string& bytes);

}  // namespace amica_engine
