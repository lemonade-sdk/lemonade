/// \file tokenizers_cpp_stub.cpp
/// \brief Stub implementation of tokenizers::Tokenizer for when tokenizers-cpp is unavailable.
/// This provides minimal functionality so the build can proceed.
#include "tokenizer/tokenizers_cpp.h"
#include <sstream>
#include <algorithm>
#include <unordered_map>

namespace tokenizers {

/// Stub implementation of Tokenizer
class TokenizerImpl : public Tokenizer {
public:
    TokenizerImpl() : vocab_size_(256) {}

    ~TokenizerImpl() override = default;

    std::vector<int32_t> Encode(const std::string& text) override {
        // Simple byte-level tokenization
        std::vector<int32_t> tokens;
        for (char c : text) {
            tokens.push_back(static_cast<uint8_t>(c));
        }
        return tokens;
    }

    std::string Decode(const std::vector<int32_t>& ids) override {
        std::string result;
        for (int32_t id : ids) {
            result += static_cast<char>(id & 0xFF);
        }
        return result;
    }

    size_t GetVocabSize() override { return vocab_size_; }

    std::string IdToToken(int32_t token_id) override {
        return std::string(1, static_cast<char>(token_id & 0xFF));
    }

    int32_t TokenToId(const std::string& token) override {
        if (token.empty()) return -1;
        return static_cast<uint8_t>(token[0]);
    }

private:
    size_t vocab_size_;
};

std::unique_ptr<Tokenizer> Tokenizer::FromBlobJSON(const std::string& json_blob) {
    return std::make_unique<TokenizerImpl>();
}

std::unique_ptr<Tokenizer> Tokenizer::FromBlobByteLevelBPE(
    const std::string& vocab_blob,
    const std::string& merges_blob,
    const std::string& added_tokens) {
    return std::make_unique<TokenizerImpl>();
}

std::unique_ptr<Tokenizer> Tokenizer::FromBlobSentencePiece(const std::string& model_blob) {
    return std::make_unique<TokenizerImpl>();
}

std::unique_ptr<Tokenizer> Tokenizer::FromBlobRWKVWorld(const std::string& model_blob) {
    return std::make_unique<TokenizerImpl>();
}

} // namespace tokenizers
