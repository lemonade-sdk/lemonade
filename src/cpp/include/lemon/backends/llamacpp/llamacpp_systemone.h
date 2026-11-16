#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lemon {
namespace backends {
namespace llamacpp {

// What lemond can say about a cut of `text` by a model of this decision type:
// false when llama-server keeps it whole or refuses it, empty when it may cut
// without saying so (lfm2-d1-omni, a long text on h2o-lightning, an unknown type).
std::optional<bool> decision_text_truncated(const std::string& decision_type,
                                            const std::string& text);

// Adds "truncated" to each answer of a /v1/systemone response to `body`, from
// decision_text_truncated of its state; adds nothing when that has no value.
void mark_answers_truncated(nlohmann::ordered_json& response, const std::string& decision_type,
                            const nlohmann::ordered_json& body);

// One question of a /v1/systemone body as a `laya` template renders it: the
// question line and each option. Laya's template is the longest rendering
// (Julia-1's drops the key or the "level i:" prefix), so counts made from it
// never undercount.
struct LayaQuestionText {
    std::string id;
    std::string question;
    std::vector<std::string> options;
};

std::vector<LayaQuestionText> laya_question_texts(const nlohmann::ordered_json& body);

// Why a `laya` model would cut this question, or "" when it reads it whole, by
// the rules of llama.cpp fill_task_laya. `option_tokens[i]` counts " " +
// options[i]; `question_tokens` counts the question line.
std::string laya_cut(const LayaQuestionText& text, const std::vector<size_t>& option_tokens,
                     size_t question_tokens, int64_t max_head_tokens);

// llama-server caps an encoder decision model's slot at its trained window, so
// a larger context buys nothing.
int encoder_decision_ctx_size(int requested, int64_t trained_window);

// Which of -b and -ub lemond adds, and at what size, given the flags in
// `user_args`; and the longest input then accepted. Without flags the batch
// stops at 4096 tokens; -b / -ub raise it, and -np above 1 without --kv-unified
// lowers it to one slot's share of the context.
struct SingleBatchPlan {
    bool add_batch_size = false;
    bool add_ubatch_size = false;
    int batch_size = 0;
    int max_input_tokens = 0;
};

SingleBatchPlan plan_single_batch(int ctx_size, const std::vector<std::string>& user_args);

nlohmann::json map_encoder_overflow_error(nlohmann::json response, int max_input_tokens);

// Why a model labelled `systemone` must not load, or "" when it may: hf_load, no
// decision head, or a decision type the pinned llama.cpp does not know. An empty
// `gguf_architecture` means lemond could not read the GGUF header.
std::string decision_model_load_refusal(const std::string& model_name, bool hf_load,
                                        const std::string& gguf_architecture,
                                        const std::string& gguf_decision_type);

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
