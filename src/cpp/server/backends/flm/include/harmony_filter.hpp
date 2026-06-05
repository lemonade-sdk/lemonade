/// \file harmony_filter.hpp
/// \brief harmony_filter class
/// \author FastFlowLM Team
/// \date 2025-10-10
/// \version 0.9.24
/// \note This class is used to filter the messages that are sent to the model
#pragma once

#include <iostream>
#include <streambuf>
#include <vector>
#include <string>

typedef enum {
    chat_template,
    reasoning,
    response
} harmony_part_t;

class harmony_filter {
private:

    // Static member definitions
    const std::string reasoning_start_marker = "<|start|>assistant<|channel|>analysis<|message|>";
    const std::string reasoning_end_marker = "<|start|>assistant<|channel|>final<|message|>";
    const std::string special_token_begin_marker = "<|";
    const std::string special_token_end_marker = "|>";

    std::string buffer;
    bool is_reasoning_part;
    int special_token_count;
    bool in_template_part;

    inline bool is_special_token(const std::string& message){
        return ((message.find(special_token_begin_marker) != std::string::npos) && (message.find(special_token_end_marker) != std::string::npos));
    }

public:
    harmony_filter(){
        this->is_reasoning_part = false;
        this->in_template_part = false;
        this->special_token_count = 0;
        buffer = "";
        buffer.reserve(256);
    }

    harmony_part_t identify_part(const std::string& message){
        harmony_part_t part;
        buffer += message;
        bool is_special = is_special_token(message);
        if (is_special){
            if (special_token_count == 0){
                in_template_part = true;
                special_token_count++;
            }
            else if (special_token_count == 2){
                in_template_part = false;
                special_token_count++;
            }
            else if (special_token_count == 3){
                in_template_part = true;
                special_token_count = 0;
            }
            else{
                special_token_count++;
            }
        }
        if ((!is_reasoning_part) && (buffer.find(reasoning_start_marker) != std::string::npos)){
            is_reasoning_part = true;
            buffer = "";
        }
        if ((is_reasoning_part) && (buffer.find(reasoning_end_marker) != std::string::npos)){
            is_reasoning_part = false;
            buffer = "";
        }

        if (in_template_part || is_special){
            part = chat_template;
        }
        else if (is_reasoning_part){
            part = reasoning;
        }
        else{
            part = response;
        }
        return part;
    }
};

/// \brief Custom streambuf that applies harmony filtering to output
class harmony_streambuf : public std::streambuf {
private:
    typedef enum {
        idle,
        send_think_start_marker,
        send_think_content,
        send_think_end_marker
    } response_state_t;

    std::unique_ptr<harmony_filter> harmony_filter_inst;
    response_state_t response_state;
    bool is_reasoning_part;
    std::vector<char> buffer;
    std::ostream& output_stream;

public:
    harmony_streambuf(std::ostream& os) : output_stream(os) {
        this->harmony_filter_inst = std::make_unique<harmony_filter>();
        this->response_state = idle;
        this->is_reasoning_part = false;
        buffer.reserve(256);
    }

protected:
    /// \brief Called when buffer is full or flush is requested
    /// \param ch the character
    /// \return the character
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            buffer.push_back(static_cast<char>(ch));
        }
        return ch;
    }

    /// \brief Called when stream is flushed
    /// \return 0
    int sync() override {
        if (!buffer.empty()) {
            std::string message(buffer.begin(), buffer.end());
            std::string filtered = filter_message(message);
            if (!filtered.empty()) {
                output_stream << filtered;
                output_stream.flush();
            }
            buffer.clear();
        }
        return 0;
    }

private:
    std::string filter_message(const std::string& message) {
        std::string filtered_message;
        harmony_part_t part = harmony_filter_inst->identify_part(message);
        if ((!is_reasoning_part) && (part == reasoning)){
            is_reasoning_part = true;
            response_state = send_think_start_marker;
        }
        else if ((is_reasoning_part) && (part == response)){
            is_reasoning_part = false;
            response_state = send_think_end_marker;
        }
        if (part == chat_template){
            filtered_message = ""; // no matter what, token with <| and |> should be ignored
        }
        else if (response_state == idle){
            filtered_message = message;
        }
        else if (response_state == send_think_start_marker){
            response_state = send_think_content;
            filtered_message = "<think>\n" + message;
        }
        else if (response_state == send_think_content){
            filtered_message = message;
        }
        else if (response_state == send_think_end_marker){
            response_state = idle;
            filtered_message = "\n</think>\n\n" + message;
        }
        else{
            response_state = idle;
            filtered_message = message;
        }

        return filtered_message;
    }
};

class cli_harmony_filter : public std::ostream {
private:
    harmony_streambuf buf;

public:
    cli_harmony_filter(std::ostream& os) : std::ostream(&buf), buf(os) {}

    /// \brief Legacy filter method for backward compatibility
    std::string filter(const std::string& message) {
        // This method is kept for backward compatibility but is not used in ostream mode
        return message;
    }
};
