/// \file tokenizer.hpp
/// \brief tokenizer class
/// \author FastFlowLM Team
/// \date 2025-08-05
/// \version 0.9.10
/// \note This class is used to tokenize the text.
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>
#include "tokenizers_cpp.h"
#include "typedef.hpp"

/// \brief Token struct
/// \param text the text
/// \param token_id the token id
typedef struct {
    std::string text;
    int token_id;
} Token;

/// \brief Role type
/// \param USER the user
/// \param ASSISTANT the assistant
/// \param SYSTEM the system
typedef enum{
    USER,
    ASSISTANT,
    SYSTEM,
    PLAIN_TEXT
} role_type;


/// \brief TokenPair struct
/// \param text the text
/// \param token_id the token id
typedef std::pair<std::string, std::string> TokenPair;

/// \brief Tokenizer class
class Tokenizer {
public:
    /// \brief Constructor
    /// \param model_path the model path
    Tokenizer(const std::string& model_path);

    /// \brief Destructor
    ~Tokenizer();

    /// \brief Encode the text
    /// \param text the text
    /// \param role the role, USER, ASSISTANT, SYSTEM, PLAIN_TEXT
    /// \return the encoded tokens
    std::vector<int> encode(const std::string& text);

    /// \brief Decode the tokens
    /// \param tokens the tokens
    /// \return the decoded text
    std::string decode(const std::vector<int>& tokens);

    /// \brief Run time decoder
    /// \param answer_token the answer token
    /// \return the decoded text
    std::string run_time_decoder(int answer_token);
    bool is_doubled_encoded;

private:
    std::unique_ptr<tokenizers::Tokenizer> tokenizer;
    std::string tokenizer_model_data_;  // Stub: stores raw tokenizer JSON
    std::unordered_map<uint32_t, uint8_t> inv_map;

    /// \brief Convert the cp1252 to utf8
    /// \param input the input string
    /// \return the utf8 string
    std::string cpt_to_utf8(const std::string& input);

    /// \brief Make the inverse byte map
    /// \return the inverse byte map
    std::unordered_map<uint32_t, uint8_t> make_inverse_byte_map();
};
