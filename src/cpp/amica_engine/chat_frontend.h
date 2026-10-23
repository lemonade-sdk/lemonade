#pragma once

#include "tokenizer.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace minja {
class TemplateNode;
}

namespace amica_engine {

struct ChatMessage {
    std::string role;  // system, user or assistant
    std::string content;
};

// Prompt formatting for a model's source directory, as the AMICA ChatApp
// frontend does it: tokenizer.json, the chat template from tokenizer_config.json
// or chat_template.jinja, and the special-token conventions of the three config
// files.
class ChatFrontend {
public:
    explicit ChatFrontend(const std::string& model_dir);
    ~ChatFrontend();

    const Tokenizer& tokenizer() const { return tokenizer_; }

    std::string render(const std::vector<ChatMessage>& messages) const;
    std::vector<std::int64_t> encode_messages(const std::vector<ChatMessage>& messages) const;

    // Tokenizes prompt text, adding BOS when tokenizer_config.json asks for it
    // and the text does not already start with it.
    std::vector<std::int64_t> encode_text(const std::string& text) const;

    const std::set<std::int64_t>& eos_ids() const { return eos_ids_; }
    const std::set<std::int64_t>& chat_stop_ids() const { return chat_stop_ids_; }

    // Harmony chats (gpt-oss) write reasoning and answer as channel messages.
    bool harmony() const { return harmony_; }

    // Muse Glimmer chats address each message to a recipient: to=self is
    // reasoning and to=user the answer.
    bool muse_glimmer() const { return muse_glimmer_; }

private:
    void resolve_stop_ids();

    Tokenizer tokenizer_;
    nlohmann::json tokenizer_config_;
    nlohmann::json generation_config_;
    nlohmann::json model_config_;
    std::set<std::string> special_tokens_;
    std::string template_text_;
    std::shared_ptr<minja::TemplateNode> template_;
    std::set<std::int64_t> eos_ids_;
    std::set<std::int64_t> chat_stop_ids_;
    bool harmony_ = false;
    bool muse_glimmer_ = false;
};

}  // namespace amica_engine
