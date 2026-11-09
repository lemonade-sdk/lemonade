#include "lemon/backends/llamacpp/llamacpp_rulehead.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace lemon {
namespace backends {
namespace llamacpp {
namespace {

constexpr const char* CATEGORIES_HEADER = "Categories:\n";
constexpr const char* TEXT_HEADER = "\n\nText:\n";
constexpr double MAX_LOGIT_SCALE = 30.0;

std::vector<float> take(const std::vector<float>& floats, size_t& offset, size_t count) {
    std::vector<float> out(floats.begin() + offset, floats.begin() + offset + count);
    offset += count;
    return out;
}

std::vector<double> pooled_projection(const nlohmann::json& rows, const std::vector<size_t>& tokens,
                                      const std::vector<float>& w, const std::vector<float>& b,
                                      int64_t hidden, int64_t proj) {
    std::vector<double> mean(static_cast<size_t>(hidden), 0.0);
    for (const size_t token : tokens) {
        const nlohmann::json& row = rows[token];
        for (int64_t d = 0; d < hidden; ++d) mean[d] += row[d].get<double>();
    }
    for (double& value : mean) value /= static_cast<double>(tokens.size());

    std::vector<double> out(static_cast<size_t>(proj), 0.0);
    double norm = 0.0;
    for (int64_t p = 0; p < proj; ++p) {
        const float* w_row = w.data() + p * hidden;
        double sum = b[p];
        for (int64_t d = 0; d < hidden; ++d) sum += w_row[d] * mean[d];
        out[p] = sum;
        norm += sum * sum;
    }
    // As F.normalize: a zero vector stays zero instead of dividing by zero.
    norm = std::max(std::sqrt(norm), 1e-12);
    for (double& value : out) value /= norm;
    return out;
}

}  // namespace

bool checkpoint_is_local(const std::string& checkpoint, bool local_origin) {
    if (local_origin || checkpoint.rfind("file://", 0) == 0) return true;
    std::error_code ec;
    return checkpoint.find("://") == std::string::npos &&
           std::filesystem::exists(std::filesystem::u8path(checkpoint), ec);
}

std::string implied_rule_head_checkpoint(const std::string& main_checkpoint, bool local_origin) {
    const size_t scheme = main_checkpoint.find("://");
    if (!checkpoint_is_local(main_checkpoint, local_origin)) {
        const size_t repo = scheme == std::string::npos ? 0 : scheme + 3;
        return main_checkpoint.substr(0, main_checkpoint.find(':', repo)) + ":" + RULE_HEAD_FILE;
    }
    const size_t slash = main_checkpoint.find_last_of("/\\");
    return (slash == std::string::npos ? std::string() : main_checkpoint.substr(0, slash + 1)) +
           RULE_HEAD_FILE;
}

std::string missing_rule_head_message(const std::string& model_name, const std::string& checkpoint) {
    return "model '" + model_name + "' is labelled 'zero-shot', but its rule head file '" +
           checkpoint + "' is missing. A llama.cpp prompt-router GGUF holds only the encoder; "
           "put the head file in the GGUF's repository, or name it as the 'rulehead' checkpoint";
}

RouterInput build_router_input(const std::vector<std::string>& labels, const std::string& text) {
    RouterInput input;
    input.text = CATEGORIES_HEADER;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) input.text += "\n";
        input.text += "- ";
        input.label_ranges.emplace_back(input.text.size(), input.text.size() + labels[i].size());
        input.text += labels[i];
    }
    input.text += TEXT_HEADER;
    input.text_start = input.text.size();
    input.text += text;
    return input;
}

std::vector<std::pair<size_t, size_t>> token_byte_spans(const nlohmann::json& tokens,
                                                        size_t text_bytes) {
    const std::runtime_error malformed("llama-server /tokenize returned no token pieces");
    if (!tokens.is_array()) throw malformed;
    std::vector<std::pair<size_t, size_t>> spans;
    spans.reserve(tokens.size());
    size_t position = 0;
    for (const auto& token : tokens) {
        if (!token.is_object() || !token.contains("piece")) throw malformed;
        const nlohmann::json& piece = token["piece"];
        size_t length = 0;
        if (piece.is_string()) {
            length = piece.get_ref<const std::string&>().size();
        } else if (piece.is_array()) {
            length = piece.size();
        } else {
            throw malformed;
        }
        spans.emplace_back(position, position + length);
        position += length;
    }
    if (position != text_bytes) {
        throw std::runtime_error("llama-server token pieces cover " + std::to_string(position) +
                                 " bytes of a " + std::to_string(text_bytes) +
                                 "-byte input, so lemond cannot tell which tokens hold each label");
    }
    return spans;
}

RouterTokens router_tokens(const nlohmann::json& with_special,
                           const nlohmann::json& without_special, size_t text_bytes) {
    const std::vector<std::pair<size_t, size_t>> spans =
        token_byte_spans(without_special, text_bytes);
    auto ids_of = [](const nlohmann::json& tokens) {
        std::vector<int64_t> ids;
        for (const auto& token : tokens) {
            const nlohmann::json& id = token.is_object() && token.contains("id") ? token["id"] : token;
            if (!id.is_number_integer()) {
                throw std::runtime_error("llama-server /tokenize returned a token without an id");
            }
            ids.push_back(id.get<int64_t>());
        }
        return ids;
    };
    if (!with_special.is_array()) {
        throw std::runtime_error("llama-server /tokenize returned no tokens");
    }
    const std::vector<int64_t> all = ids_of(with_special);
    const std::vector<int64_t> text = ids_of(without_special);

    for (size_t lead = 0; lead + text.size() <= all.size(); ++lead) {
        if (!std::equal(text.begin(), text.end(), all.begin() + lead)) continue;
        RouterTokens out;
        out.ids = all;
        out.spans.assign(lead, {0, 0});
        out.spans.insert(out.spans.end(), spans.begin(), spans.end());
        out.spans.resize(all.size(), {text_bytes, text_bytes});
        return out;
    }
    throw std::runtime_error("llama-server tokenized the input differently with and without its "
                             "special tokens, so lemond cannot place them");
}

RouterTokenGroups group_router_tokens(const RouterInput& input,
                                      const std::vector<std::pair<size_t, size_t>>& spans,
                                      const std::vector<std::string>& labels) {
    RouterTokenGroups groups;
    groups.labels.resize(input.label_ranges.size());
    for (size_t i = 0; i < spans.size(); ++i) {
        const auto [start, end] = spans[i];
        if (start == end) continue;
        for (size_t r = 0; r < input.label_ranges.size(); ++r) {
            const auto [label_start, label_end] = input.label_ranges[r];
            if (start < label_end && end > label_start) groups.labels[r].push_back(i);
        }
        if (end > input.text_start) groups.text.push_back(i);
    }
    for (size_t r = 0; r < groups.labels.size(); ++r) {
        if (groups.labels[r].empty()) {
            throw std::invalid_argument("label '" + (r < labels.size() ? labels[r] : "") +
                                        "' holds nothing this tokenizer keeps");
        }
    }
    if (groups.text.empty()) {
        throw std::invalid_argument("the input holds nothing this tokenizer keeps");
    }
    return groups;
}

RuleHead parse_rule_head(const std::string& bytes, int64_t hidden) {
    if (hidden <= 0) {
        throw std::runtime_error("the encoder's embedding length is unknown, so lemond cannot "
                                 "read its rule head");
    }
    const size_t n = bytes.size() / sizeof(float);
    const size_t per_proj = 2 * static_cast<size_t>(hidden) + 2;
    if (bytes.size() % sizeof(float) != 0 || n < 2 || (n - 2) % per_proj != 0 ||
        (n - 2) / per_proj == 0) {
        throw std::runtime_error("the rule head is " + std::to_string(bytes.size()) +
                                 " bytes, which is no head for an encoder of width " +
                                 std::to_string(hidden));
    }
    std::vector<float> floats(n);
    std::memcpy(floats.data(), bytes.data(), n * sizeof(float));
    if (!std::all_of(floats.begin(), floats.end(), [](float value) { return std::isfinite(value); })) {
        throw std::runtime_error("the rule head contains a value that is not finite");
    }

    RuleHead head;
    head.hidden = hidden;
    head.proj = static_cast<int64_t>((n - 2) / per_proj);
    const size_t matrix = static_cast<size_t>(head.proj * hidden);
    size_t offset = 0;
    head.tok_w = take(floats, offset, matrix);
    head.tok_b = take(floats, offset, static_cast<size_t>(head.proj));
    head.rule_w = take(floats, offset, matrix);
    head.rule_b = take(floats, offset, static_cast<size_t>(head.proj));
    head.score_bias = floats[offset];
    head.logit_scale = floats[offset + 1];
    return head;
}

std::vector<double> score_router_labels(const RuleHead& head, const nlohmann::json& rows,
                                        size_t n_tokens, const RouterTokenGroups& groups) {
    if (!rows.is_array() || rows.size() != n_tokens) {
        throw std::runtime_error("llama-server returned " +
                                 std::to_string(rows.is_array() ? rows.size() : 0) +
                                 " token vectors for " + std::to_string(n_tokens) + " tokens");
    }
    auto check_rows = [&](const std::vector<size_t>& tokens) {
        for (const size_t token : tokens) {
            const nlohmann::json& row = rows[token];
            if (!row.is_array() || static_cast<int64_t>(row.size()) != head.hidden) {
                throw std::runtime_error("llama-server returned a token vector of width " +
                                         std::to_string(row.is_array() ? row.size() : 0) +
                                         " for a rule head of width " +
                                         std::to_string(head.hidden));
            }
        }
    };
    check_rows(groups.text);
    for (const auto& label : groups.labels) check_rows(label);

    const std::vector<double> query =
        pooled_projection(rows, groups.text, head.tok_w, head.tok_b, head.hidden, head.proj);
    const double scale = std::min(std::exp(static_cast<double>(head.logit_scale)), MAX_LOGIT_SCALE);

    std::vector<double> logits;
    logits.reserve(groups.labels.size());
    for (const auto& label : groups.labels) {
        const std::vector<double> rule =
            pooled_projection(rows, label, head.rule_w, head.rule_b, head.hidden, head.proj);
        double cosine = 0.0;
        for (size_t p = 0; p < rule.size(); ++p) cosine += query[p] * rule[p];
        logits.push_back(cosine * scale + head.score_bias);
    }

    const double max_logit = *std::max_element(logits.begin(), logits.end());
    double sum = 0.0;
    for (double& value : logits) {
        value = std::exp(value - max_logit);
        sum += value;
    }
    for (double& value : logits) value /= sum;
    return logits;
}

nlohmann::json router_classify_response(const std::vector<std::string>& labels,
                                        const std::vector<double>& probabilities, int top_k) {
    std::vector<size_t> order(labels.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return probabilities[a] > probabilities[b];
    });
    if (top_k > 0 && static_cast<size_t>(top_k) < order.size()) {
        order.resize(static_cast<size_t>(top_k));
    }
    nlohmann::json scores = nlohmann::json::object();
    for (const size_t i : order) scores[labels[i]] = probabilities[i];
    return nlohmann::json{{"labels", std::move(scores)}, {"truncated", false}};
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
