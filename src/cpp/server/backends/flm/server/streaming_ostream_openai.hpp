/*!
 *  Copyright (c) 2023 by Contributors
 * \file streaming_ostream.hpp
 * \brief Custom ostream for streaming
 * \author FastFlowLM Team
 * \date 2025-06-24
 *  \version 0.9.24
 */
#pragma once

#include <ostream>
#include <streambuf>
#include <functional>
#include <string>
#include <vector>
#include <random>
#include <sstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include "AutoModel/automodel.hpp"
#include "harmony_filter.hpp"

using json = nlohmann::ordered_json;


class AutoModel;

// v1/completions
///@brief Custom streambuf that captures tokens and sends them immediately
///@param model the model
///@param callback the callback
///@param is_chat_format the is chat format
///@return the streaming buf
///@note The packet is sent every std::flush, but only when UTF-8 sequences are complete
class streaming_buf_openai : public std::streambuf {
public:
    ///@brief StreamCallback
    using StreamCallback = std::function<void(const std::string&, bool)>;

    streaming_buf_openai(const std::string& model, StreamCallback callback)
        : model_name(model), stream_callback(callback), first_chunk(true) {
        // Generate a unique ID for this stream
        generate_stream_id();
        generate_created();
        generate_system_fingerprint();
    }

protected:
    ///@brief Called when buffer is full or flush is requested
    ///@param ch the character
    ///@return the character
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            buffer += static_cast<char>(ch);
        }
        return ch;
    }

    ///@brief Called when stream is flushed
    ///@return 0
    int sync() override {
        flush_complete_utf8_sequences(false);
        return 0;
    }

public:
    ///@brief Call this when generation is complete
    void finalize(chat_meta_info_t& meta_info) {
        // Send all remaining content, including incomplete sequences
        if (!buffer.empty()) {
            send_response(buffer, true);
            buffer.clear();
        }
        send_final_response(meta_info);
    }


private:
    ///@brief Generate a unique stream ID
    void generate_stream_id() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);

        std::stringstream ss;
        ss << "chatcmpl-";
        for (int i = 0; i < 24; ++i) {
            ss << std::hex << dis(gen);
        }
        stream_id = ss.str();
    }
    void generate_created() {
        created = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
    void generate_system_fingerprint() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);

        std::stringstream ss;
        ss << "fp_";
        for (int i = 0; i < 16; ++i) {
            ss << std::hex << dis(gen);
        }
        system_fingerprint = ss.str();
    }

    ///@brief Get UTF-8 sequence length from first byte
    ///@param first_byte the first byte
    ///@return sequence length, or 0 if invalid
    size_t get_utf8_sequence_length(unsigned char first_byte) {
        if ((first_byte & 0x80) == 0) {
            return 1; // Single byte sequence
        } else if ((first_byte & 0xE0) == 0xC0) {
            return 2; // Two byte sequence
        } else if ((first_byte & 0xF0) == 0xE0) {
            return 3; // Three byte sequence
        } else if ((first_byte & 0xF8) == 0xF0) {
            return 4; // Four byte sequence
        }
        return 0; // Invalid UTF-8 start byte
    }

    ///@brief Flush only complete UTF-8 sequences
    ///@param is_final the is final
    void flush_complete_utf8_sequences(bool is_final) {
        if (buffer.empty()) return;

        std::string complete_content;
        size_t pos = 0;

        // Process complete UTF-8 sequences
        while (pos < buffer.size()) {
            unsigned char first = static_cast<unsigned char>(buffer[pos]);
            size_t seq_len = get_utf8_sequence_length(first);

            if (seq_len == 0) {
                // Invalid UTF-8 start byte, skip it
                pos++;
                continue;
            }

            // Check if we have a complete sequence
            if (pos + seq_len > buffer.size()) {
                // Incomplete sequence, stop here
                break;
            }

            // Add complete sequence to output
            complete_content.append(buffer, pos, seq_len);
            pos += seq_len;
        }

        // Send complete sequences if any
        if (!complete_content.empty()) {
            send_response(complete_content, is_final);
        }

        // Remove processed bytes from buffer
        if (pos > 0) {
            buffer.erase(0, pos);
        }
    }

    ///@brief Send the response
    ///@param content the content
    ///@param is_final the is final
    void send_response(const std::string& content, bool is_final) {
        json response;

        // Content chunk
        response = {
            {"id", stream_id},
            {"object", "text_completion"},
            {"created", created},
            {"system_fingerprint", system_fingerprint},
            {"model", model_name},
            {"choices", json::array({
                {
                    {"text", content},
                    {"index", 0},
                    {"logprobs", nullptr},
                    {"finish_reason", nullptr}
                }
            })}
        };

        stream_callback("data: " + response.dump() + "\n\n", is_final);

    }

    ///@brief Send the chat final response
    void send_final_response(chat_meta_info_t& meta_info) {
        // Send final chunk with finish_reason only (no empty delta)
        json final_response = {
            {"id", stream_id},
            {"object", "text_completion"},
            {"created", created},
            {"system_fingerprint", system_fingerprint},
            {"model", model_name},
            {"choices", json::array({
                {
                    {"finish_reason", stop_reason_to_string(meta_info.stop_reason)},
                }
            })},
            {"usage", {
                {"prompt_tokens", meta_info.prompt_tokens},
                {"completion_tokens", meta_info.generated_tokens},
                {"total_tokens", meta_info.prompt_tokens + meta_info.generated_tokens}
            }}
        };
        stream_callback("data: " + final_response.dump() + "\n\n", false);
        // Send the [DONE] message
        stream_callback("data: [DONE]\n\n", true);
    }


    ///@brief Buffer
    std::string buffer;
    ///@brief Model name
    std::string model_name;
    ///@brief Stream callback
    StreamCallback stream_callback;
    ///@brief Stream ID
    std::string stream_id;
    long created;
    std::string system_fingerprint;
    ///@brief First chunk flag
    bool first_chunk;
};

///@brief Custom ostream for streaming
///@param model the model
///@param callback the callback
///@param is_chat_format the is chat format
///@return the streaming ostream
class streaming_ostream_openai : public std::ostream {
public:
    streaming_ostream_openai(const std::string& model, streaming_buf_openai::StreamCallback callback)
        : std::ostream(&buf), buf(model, callback) {}

    ///@brief Finalize the chat
    void finalize(chat_meta_info_t& meta_info) {
        buf.finalize(meta_info);
    }

private:
    ///@brief Buffer
    streaming_buf_openai buf;
};



// v1/chat/completions
///@brief Custom streambuf that captures tokens and sends them immediately
///@param model the model
///@param callback the callback
///@param is_chat_format the is chat format
///@return the streaming buf
///@note The packet is sent every std::flush, but only when UTF-8 sequences are complete
class streaming_buf_openai_chat : public std::streambuf {
public:
    ///@brief StreamCallback
    using StreamCallback = std::function<void(const std::string&, bool)>;

    streaming_buf_openai_chat(const std::string& model, AutoModel* auto_chat_engine, StreamCallback callback)
        : model_name(model), auto_chat_engine(auto_chat_engine), stream_callback(callback), first_chunk(true) {
        // Generate a unique ID for this stream
        generate_stream_id();
        generate_created();
        generate_system_fingerprint();
        harmony_filter_inst = std::make_unique<harmony_filter>();
    }

protected:
    ///@brief Called when buffer is full or flush is requested
    ///@param ch the character
    ///@return the character
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            buffer += static_cast<char>(ch);
        }
        return ch;
    }

    ///@brief Called when stream is flushed
    ///@return 0
    int sync() override {
        flush_complete_utf8_sequences(false);
        return 0;
    }

public:
    ///@brief Call this when generation is complete
    void finalize(chat_meta_info_t& meta_info) {
        // Send all remaining content, including incomplete sequences
        if (!buffer.empty()) {
            send_response(buffer, false, true);
            buffer.clear();
        }
        else {
            send_response("", false, true);
        }
        if (stream_stop_reason == stop_reason_t::TOOL_DETECTED) {
            meta_info.stop_reason = stream_stop_reason;
        }
        send_final_response(meta_info);
    }


private:
    ///@brief Generate a unique stream ID
    void generate_stream_id() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);

        std::stringstream ss;
        ss << "chatcmpl-";
        for (int i = 0; i < 24; ++i) {
            ss << std::hex << dis(gen);
        }
        stream_id = ss.str();
    }

    void generate_created() {
        created = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }


    void generate_system_fingerprint() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);

        std::stringstream ss;
        ss << "fp_";
        for (int i = 0; i < 16; ++i) {
            ss << std::hex << dis(gen);
        }
        system_fingerprint = ss.str();
    }

    void tool_calling_id() {

    }

    ///@brief Get UTF-8 sequence length from first byte
    ///@param first_byte the first byte
    ///@return sequence length, or 0 if invalid
    size_t get_utf8_sequence_length(unsigned char first_byte) {
        if ((first_byte & 0x80) == 0) {
            return 1; // Single byte sequence
        }
        else if ((first_byte & 0xE0) == 0xC0) {
            return 2; // Two byte sequence
        }
        else if ((first_byte & 0xF0) == 0xE0) {
            return 3; // Three byte sequence
        }
        else if ((first_byte & 0xF8) == 0xF0) {
            return 4; // Four byte sequence
        }
        return 0; // Invalid UTF-8 start byte
    }

    ///@brief Flush only complete UTF-8 sequences
    ///@param is_final the is final
    void flush_complete_utf8_sequences(bool is_final) {
        if (buffer.empty()) return;

        std::string complete_content;
        size_t pos = 0;

        // Process complete UTF-8 sequences
        while (pos < buffer.size()) {
            unsigned char first = static_cast<unsigned char>(buffer[pos]);
            size_t seq_len = get_utf8_sequence_length(first);

            if (seq_len == 0) {
                // Invalid UTF-8 start byte, skip it
                pos++;
                continue;
            }

            // Check if we have a complete sequence
            if (pos + seq_len > buffer.size()) {
                // Incomplete sequence, stop here
                break;
            }

            // Add complete sequence to output
            complete_content.append(buffer, pos, seq_len);
            pos += seq_len;
        }

        // Send complete sequences if any
        if (!complete_content.empty()) {
            send_response(complete_content, is_final);
        }

        // Remove processed bytes from buffer
        if (pos > 0) {
            buffer.erase(0, pos);
        }
    }

    ///@brief Send the response
    ///@param content the content
    ///@param is_final the is final
    void send_response(const std::string& content, bool is_final, bool parser_final = false) {
        static int index = 0;
        json response;

        StreamResult result = parser_final
            ? auto_chat_engine->parse_stream_content_final(content)
            : auto_chat_engine->parse_stream_content(content);
        if (result.type == StreamEventType::WAITING) {
            return;
        }
        json delta;
        if (result.type == StreamEventType::TOOL_DONE) {
            stream_stop_reason = stop_reason_t::TOOL_DETECTED;
            delta = {
                {"tool_calls", json::array({
                    {
                        {"index", index++},
                        {"id", result.tool_id},
                        {"type", "function"},
                        {"function", {
                            {"name", result.tool_name},
                            {"arguments", result.tool_args_str}
                        }}
                    }
                })}
            };
            header_print("FLM", "Tool name: " + result.tool_name);
            header_print("FLM", "Tool args: " + result.tool_args_str);
            header_print("FLM", "Tool JSON: " + delta["tool_calls"][0]["function"].dump());
        }
        else if (result.type == StreamEventType::REASONING) {
            delta = {
                {"role", "assistant"},
                {"reasoning_content", result.content}
            };
        }
        else if (result.type == StreamEventType::CONTENT) {
            if (result.content.empty()) return;

            delta = {
                {"role", "assistant"},
                {"content", result.content}
            };
        }

        response = {
            {"id", stream_id},
            {"object", "chat.completion.chunk"},
            {"created", created},
            {"model", model_name},
            {"choices", json::array({
                {
                    {"index", 0},
                    {"delta", delta},
                    {"finish_reason", nullptr}
                }
            })}
        };

        stream_callback("data: " + response.dump() + "\n\n", is_final);
    }

    ///@brief Send the chat final response
    void send_final_response(chat_meta_info_t& meta_info) {
        json final_response = {
            {"id", stream_id},
            {"object", "chat.completion.chunk"},
            {"created", created},
            {"model", model_name},
            {"system_fingerprint", system_fingerprint},
            {"choices", json::array({
                {
                    {"index", 0},
                    {"delta", {
                    {"content", nullptr}
                }},
                    //{"logprobs", nullptr},
                    {"finish_reason", stop_reason_to_string(meta_info.stop_reason)}
                }
            })},
            {"usage", {
                {"prompt_tokens", meta_info.prompt_tokens},
                {"completion_tokens", meta_info.generated_tokens},
                {"total_tokens", meta_info.prompt_tokens + meta_info.generated_tokens},
                {"active_kv_tokens", this->auto_chat_engine->get_current_context_length()},
                {"max_kv_token_capacity", this->auto_chat_engine->get_max_length()},
                {"kv_token_occupancy_rate_percentage", (float)this->auto_chat_engine->get_current_context_length() / (float)this->auto_chat_engine->get_max_length() * 100},
                {"load_duration", static_cast<double>(meta_info.load_duration) / 1'000'000'000},
                {"prefill_duration_ttft", static_cast<double>(meta_info.prefill_duration) / 1'000'000'000},
                {"decoding_duration", static_cast<double>(meta_info.decoding_duration) / 1'000'000'000},
                {"prefill_speed_tps", static_cast<double>(meta_info.prompt_tokens) / static_cast<double>(meta_info.prefill_duration) * 1'000'000'000},
                {"decoding_speed_tps", static_cast<double>(meta_info.generated_tokens) / static_cast<double>(meta_info.decoding_duration) * 1'000'000'000},
            }}
        };
        stream_callback("data: " + final_response.dump() + "\n\n", false);
        std::cout << "ChatCompletionChunk: " << final_response << std::endl;
        // Send the [DONE] message
        stream_callback("data: [DONE]\n\n", true);
    }


    ///@brief Buffer
    std::string buffer;
    ///@brief Model name
    std::string model_name;
    ///@brief auto_chat_engine
    AutoModel* auto_chat_engine;
    ///@brief Stream callback
    StreamCallback stream_callback;
    ///@brief Stream ID
    std::string stream_id;
    long created;
    std::string system_fingerprint;
    ///@brief First chunk flag
    bool first_chunk;
    stop_reason_t stream_stop_reason = stop_reason_t::EOT_DETECTED;

    std::unique_ptr<harmony_filter> harmony_filter_inst;
};

///@brief Custom ostream for streaming
///@param model the model
///@param callback the callback
///@param is_chat_format the is chat format
///@return the streaming ostream
class streaming_ostream_openai_chat : public std::ostream {
public:
    streaming_ostream_openai_chat(const std::string& model, AutoModel* auto_chat_engine, streaming_buf_openai_chat::StreamCallback callback)
        : std::ostream(&buf), buf(model, auto_chat_engine, callback) {}

    ///@brief Finalize the chat
    void finalize(chat_meta_info_t& meta_info) {
        buf.finalize(meta_info);
    }

private:
    ///@brief Buffer
    streaming_buf_openai_chat buf;
};
