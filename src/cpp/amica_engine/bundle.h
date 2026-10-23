#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace amica_engine {

// An AMICA ChatApp bundle: its graphs, weights and tokenizer, described by
// amica_app.json when ChatApp has prepared one and derived otherwise.
class Bundle {
public:
    explicit Bundle(const std::string& model_dir);

    // The generation plan the Engine runs: graphs, weights and input roles.
    const nlohmann::json& plan() const { return plan_; }

    // Directory holding the tokenizer, chat template and configs.
    const std::string& source_dir() const { return source_dir_; }

    // Whether prompts are rendered with the chat template (otherwise the
    // messages are joined as plain text).
    bool chat_template() const { return chat_template_; }

    const std::string& prompt_padding() const { return prompt_padding_; }

    // True when the bundle had no amica_app.json and its manifest was derived
    // from the bundle's own files.
    bool manifest_derived() const { return manifest_derived_; }
    std::size_t max_prompt_tokens() const { return prefill_.back().capacity; }

    // The most tokens, prompt plus generated, one request can hold; unknown
    // for bundles whose cache layout does not say.
    std::optional<std::size_t> context_capacity() const { return context_capacity_; }

private:
    struct Prefill {
        std::string graph;
        std::size_t capacity;
        nlohmann::json cache_map;
    };

    std::string prefill_input() const;
    std::size_t graph_capacity(const std::string& graph) const;
    void load_prefill_graphs();
    void build_plan();

    nlohmann::json manifest_;
    bool manifest_derived_ = false;
    nlohmann::json generation_;
    std::string manifest_path_;
    std::string source_dir_;
    std::string decoder_;
    bool self_contained_ = false;
    bool chat_template_ = false;
    std::string prompt_padding_;
    std::vector<Prefill> prefill_;
    std::optional<std::size_t> context_capacity_;
    nlohmann::json plan_;
};

}  // namespace amica_engine
