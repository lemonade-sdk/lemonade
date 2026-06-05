/// \file tokenizer.cpp
/// \brief Tokenizer implementation for text encoding/decoding
/// \author FastFlowLM Team (adapted for Lemonade)
/// \date 2025-06-24
/// \version 0.9.10
#include "tokenizer/tokenizer.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <regex>

// Minimal stub tokenizer for when tokenizers-cpp is unavailable.
// This provides basic tokenization using a simple byte-level approach.
// The real implementation uses tokenizers::Tokenizer from tokenizers-cpp.

/// \brief Constructor
/// \param model_path the model path
Tokenizer::Tokenizer(const std::string& model_path) {
    #ifdef _WIN32
    std::ifstream fs(model_path + "\\tokenizer.json", std::ios::in | std::ios::binary);
    #else
    std::ifstream fs(model_path + "/tokenizer.json", std::ios::in | std::ios::binary);
    #endif
    if (fs.fail()) {
        #ifdef _WIN32
        std::cerr << "Cannot open " << model_path + "\\tokenizer.json" << std::endl;
        #else
        std::cerr << "Cannot open " << model_path + "/tokenizer.json" << std::endl;
        #endif
        exit(1);
    }
    std::string data;
    fs.seekg(0, std::ios::end);
    size_t size = static_cast<size_t>(fs.tellg());
    fs.seekg(0, std::ios::beg);
    data.resize(size);
    fs.read(data.data(), size);
    fs.close();

    // Parse JSON to check decoder type (same as original)
    nlohmann::json data_json = nlohmann::json::parse(data);
    std::string decoder_type;
    if (data_json.contains("decoder") && data_json["decoder"].contains("type")) {
        decoder_type = data_json["decoder"]["type"].get<std::string>();
    }
    if (decoder_type == "ByteLevel") {
        this->is_doubled_encoded = true;
    }
    else {
        this->is_doubled_encoded = false;
    }

    // Stub: load tokenizer model from JSON
    // In the real implementation, this calls tokenizers::Tokenizer::FromBlobJSON(data)
    // For now, we store the raw data for later use
    this->tokenizer_model_data_ = data;
}

/// \brief Destructor
Tokenizer::~Tokenizer() = default;

/// \brief Make the inverse byte map
/// \return the inverse byte map
std::unordered_map<uint32_t, uint8_t> Tokenizer::make_inverse_byte_map() {
    // 1) Build the "printable" byte list bs exactly as in GPT-2/LLaMA
    std::vector<uint8_t> bs;
    for (int b = 33; b <= 126; b++) bs.push_back((uint8_t)b);
    for (int b = 161; b <= 172; b++) bs.push_back((uint8_t)b);
    for (int b = 174; b <= 255; b++) bs.push_back((uint8_t)b);

    // 2) Build the codepoint list cs (same length, but extended to 256)
    std::vector<uint32_t> cs(bs.begin(), bs.end());
    int n = 0;
    for (int b = 0; b < 256; b++) {
        if (std::find(bs.begin(), bs.end(), (uint8_t)b) == bs.end()) {
            bs.push_back((uint8_t)b);
            cs.push_back(256 + n);
            n++;
        }
    }

    // 3) Zip them into an inverse map: cp -> original byte
    std::unordered_map<uint32_t, uint8_t> inv;
    for (size_t i = 0; i < bs.size(); i++) {
        inv[ cs[i] ] = bs[i];
    }
    return inv;
}

/// \brief Convert the codepoint to utf8
/// \param input the input
/// \return the utf8 string
std::string Tokenizer::cpt_to_utf8(const std::string& input) {
    static const std::string pattern = "\xe2\x96\x81"; // \xe2\x96\x81 is "▁" in UTF-8
    static const size_t pattern_size = 3;
    static auto inv_map = make_inverse_byte_map();
    std::string output = "";
    output.reserve(input.size());
    size_t i = 0;
    if (!this->is_doubled_encoded) { // simply do pattern substitution of "\xe2\x96\x81" to " ", temporary solution
        std::string output2 = "";
        for (size_t i = 0; i < input.size(); i++) {
            if (i <= input.size() - pattern_size && input.substr(i, pattern_size) == pattern) {
                output2 += " ";
                i += pattern_size - 1; // -1 because the loop will increment i by 1
            }
            else {
                output2 += input[i];
            }
        }
        return output2;
    }
    while (i < input.size()) {
        unsigned char c = input[i];
        uint32_t cp;
        size_t width;

        // --- Decode a UTF-8 codepoint ---
        if (c < 0x80) {
            cp = c;
            width = 1;
        }
        else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= input.size()) throw std::runtime_error("Truncated UTF-8, ll = 2");
            cp  = (uint32_t)(c     & 0x1F) << 6;
            cp |= (uint32_t)(input[i+1] & 0x3F);
            width = 2;
        }
        else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= input.size()) throw std::runtime_error("Truncated UTF-8, ll = 3");
            cp  = (uint32_t)(c     & 0x0F) << 12;
            cp |= (uint32_t)(input[i+1] & 0x3F) << 6;
            cp |= (uint32_t)(input[i+2] & 0x3F);
            width = 3;
        }
        else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= input.size()) throw std::runtime_error("Truncated UTF-8, ll = 4");
            cp  = (uint32_t)(c     & 0x07) << 18;
            cp |= (uint32_t)(input[i+1] & 0x3F) << 12;
            cp |= (uint32_t)(input[i+2] & 0x3F) << 6;
            cp |= (uint32_t)(input[i+3] & 0x3F);
            width = 4;
        }
        else {
            std::cout << std::hex <<"cp: " << cp << std::endl;
            throw std::runtime_error("Invalid UTF-8 byte");
        }

        // --- Look up the original byte ---
        auto it = inv_map.find(cp);
        if (it == inv_map.end()) {
            throw std::runtime_error("Codepoint not in LLaMA byte map");
        }
        output.push_back((char)it->second);

        i += width;
    }

    return output;
}

/// \brief Encode the text
/// \param text the text
/// \return the encoded tokens
std::vector<int> Tokenizer::encode(const std::string& text) {
    // Stub: simple character-level tokenization
    // The real implementation calls this->tokenizer->Encode(text)
    std::vector<int> tokens;
    for (char c : text) {
        tokens.push_back(static_cast<uint8_t>(c));
    }
    return tokens;
}

/// \brief Decode the tokens
/// \param tokens the tokens
/// \return the decoded text
std::string Tokenizer::decode(const std::vector<int>& tokens) {
    // Stub: simple character-level decoding
    // The real implementation calls this->tokenizer->Decode(tokens)
    std::string result;
    for (int t : tokens) {
        result += static_cast<char>(t & 0xFF);
    }
    return result;
}

/// \brief Run time decoder
/// \param answer_token the answer token
/// \return the decoded text
std::string Tokenizer::run_time_decoder(int answer_token) {
    // Stub: convert token to character
    // The real implementation calls this->cpt_to_utf8(this->tokenizer->IdToToken(answer_token))
    return cpt_to_utf8(std::string(1, static_cast<char>(answer_token & 0xFF)));
}
