#include "lemon/backends/llamacpp/llamacpp_rulehead.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <set>
#include <stdexcept>

namespace lemon {
namespace backends {
namespace llamacpp {
namespace {

constexpr const char* CATEGORIES_HEADER = "Categories:\n";
constexpr const char* TEXT_HEADER = "\n\nText:\n";
constexpr double MAX_LOGIT_SCALE = 30.0;

// ort-server 0.4.0's constants (src/main.cpp), which the fit must keep in step with.
constexpr const char* ELISION = "\n...\n";
constexpr size_t TEXT_HEAD_DIVISOR = 3;
constexpr size_t MIN_TEXT_TOKENS = 8;
constexpr int MAX_REFIT_ATTEMPTS = 8;
// ort-server pre-cuts by characters at budget x max(longest vocabulary token, 32).
// LFM2's longest token is 23 characters, so ort-server uses the floor, 32, too.
constexpr size_t MAX_TOKEN_CHARS = 32;

bool is_char_boundary(const std::string& s, size_t pos) {
    return pos >= s.size() || (static_cast<unsigned char>(s[pos]) & 0xC0) != 0x80;
}

size_t utf8_length(const std::string& s) {
    size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

size_t utf8_byte_at(const std::string& s, size_t chars) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (is_char_boundary(s, i)) {
            if (n == chars) return i;
            ++n;
        }
    }
    return s.size();
}

std::string utf8_head_tail(const std::string& s, size_t max_chars) {
    const size_t total = utf8_length(s);
    if (total <= max_chars) return s;
    const size_t head = max_chars / TEXT_HEAD_DIVISOR;
    const size_t tail = max_chars - head;
    return s.substr(0, utf8_byte_at(s, head)) + s.substr(utf8_byte_at(s, total - tail));
}

// ort-server's head_chars_for / tail_chars_for, on byte spans: a cut that would
// land inside a character moves to the side that drops the whole character.
size_t head_end_for(const std::vector<std::pair<size_t, size_t>>& spans, size_t budget,
                    const std::string& s) {
    size_t h = std::min(budget, spans.size());
    while (h > 0 && h < spans.size() && !is_char_boundary(s, spans[h - 1].second)) --h;
    return h > 0 ? spans[h - 1].second : 0;
}

size_t tail_start_for(const std::vector<std::pair<size_t, size_t>>& spans, size_t budget,
                      const std::string& s) {
    if (budget == 0 || spans.empty()) return s.size();
    size_t i = spans.size() - std::min(budget, spans.size());
    while (i < spans.size() && i > 0 && !is_char_boundary(s, spans[i].first)) ++i;
    return i < spans.size() ? spans[i].first : s.size();
}

std::vector<float> take(const std::vector<float>& floats, size_t& offset, size_t count) {
    std::vector<float> out(floats.begin() + offset, floats.begin() + offset + count);
    offset += count;
    return out;
}

std::vector<double> mean_of_rows(const nlohmann::json& rows, const std::vector<size_t>& tokens,
                                 int64_t hidden) {
    std::vector<double> mean(static_cast<size_t>(hidden), 0.0);
    for (const size_t token : tokens) {
        const nlohmann::json& row = rows[token];
        for (int64_t d = 0; d < hidden; ++d) mean[d] += row[d].get<double>();
    }
    for (double& value : mean) value /= static_cast<double>(tokens.size());
    return mean;
}

std::vector<double> row_of(const nlohmann::json& row) {
    std::vector<double> out;
    out.reserve(row.size());
    for (const auto& value : row) out.push_back(value.get<double>());
    return out;
}

void check_row_width(const nlohmann::json& row, int64_t hidden) {
    if (!row.is_array() || static_cast<int64_t>(row.size()) != hidden) {
        throw std::runtime_error("llama-server returned a vector of width " +
                                 std::to_string(row.is_array() ? row.size() : 0) +
                                 " for a rule head of width " + std::to_string(hidden));
    }
}

std::vector<double> projection(const std::vector<double>& mean, const std::vector<float>& w,
                               const std::vector<float>& b, int64_t hidden, int64_t proj) {
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

std::vector<double> score_means(const RuleHead& head, const std::vector<double>& text_mean,
                                const std::vector<std::vector<double>>& label_means) {
    const std::vector<double> query =
        projection(text_mean, head.tok_w, head.tok_b, head.hidden, head.proj);
    const double scale = std::min(std::exp(static_cast<double>(head.logit_scale)), MAX_LOGIT_SCALE);

    std::vector<double> logits;
    logits.reserve(label_means.size());
    for (const auto& label_mean : label_means) {
        const std::vector<double> rule =
            projection(label_mean, head.rule_w, head.rule_b, head.hidden, head.proj);
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

}  // namespace

bool checkpoint_is_local(const std::string& checkpoint, bool local_origin) {
    if (local_origin || checkpoint.rfind("file://", 0) == 0) return true;
    return checkpoint.find("://") == std::string::npos &&
           std::filesystem::u8path(checkpoint).has_root_directory();
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

std::string local_directory_checkpoint_error(const std::string& model_name,
                                             const std::string& path) {
    const std::string bare = path.rfind("file://", 0) == 0 ? path.substr(7) : path;
    std::error_code ec;
    if (bare.empty() || !std::filesystem::is_directory(std::filesystem::u8path(bare), ec)) {
        return "";
    }
    return "model '" + model_name + "' names the folder '" + bare + "' as its checkpoint, but "
           "llama-server loads one .gguf file. Name the file, such as the .gguf in a Hugging "
           "Face cache's snapshots/<revision>/ folder; its rulehead.bin goes beside it";
}

std::string mixed_rule_head_source_error(const std::string& model_name,
                                         const std::string& main_checkpoint,
                                         const std::string& rule_head, bool local_origin) {
    if (rule_head.empty()) return "";
    const bool main_local = checkpoint_is_local(main_checkpoint, local_origin);
    if (main_local == checkpoint_is_local(rule_head, local_origin)) return "";
    return "model '" + model_name + "' has a " + (main_local ? "local" : "remote") +
           " GGUF but a " + (main_local ? "remote" : "local") + " 'rulehead' checkpoint '" +
           rule_head + "'. Lemonade reads both from the same place: put rulehead.bin " +
           (main_local ? "on disk, beside the GGUF or at a path named as the 'rulehead' "
                         "checkpoint"
                       : "in a repository, named as `<repo>:rulehead.bin`");
}

int rule_head_ctx_size(int requested) {
    return requested > 0 ? requested : RULE_HEAD_ROUTER_CTX_SIZE;
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

RouterFit fit_router_input(const std::vector<std::string>& labels, const std::string& text,
                           size_t window, size_t n_specials, size_t elision_tokens,
                           const PieceTokenizer& tokenize) {
    const RouterInput block = build_router_input(labels, "");
    // As ort-server: a block the window provably cannot hold is refused by one
    // division, before a label set of any size is sent to the tokenizer.
    const size_t block_chars = utf8_length(block.text);
    if (block_chars / MAX_TOKEN_CHARS > window) {
        throw std::invalid_argument(
            "the label list is far larger than this model's " + std::to_string(window) +
            "-token window (" + std::to_string(block_chars) +
            " characters of labels). Send fewer or shorter labels.");
    }
    const size_t label_tokens = token_byte_spans(tokenize(block.text), block.text.size()).size();
    const size_t min_text = std::max<size_t>(1, std::min(MIN_TEXT_TOKENS, window / 8));
    const size_t reserve = n_specials + min_text;
    if (label_tokens + reserve > window) {
        throw std::invalid_argument(
            "the label list needs " + std::to_string(label_tokens) +
            " tokens, and this model's window holds " + std::to_string(window) + " with " +
            std::to_string(reserve) +
            " reserved for markers and text. Send fewer or shorter labels.");
    }
    const size_t text_budget = window - n_specials - label_tokens;

    RouterFit fit;
    std::string body = text;
    size_t mark_from = 0;
    size_t mark_to = 0;
    if (utf8_length(body) > text_budget * MAX_TOKEN_CHARS) {
        body = utf8_head_tail(body, text_budget * MAX_TOKEN_CHARS);
        fit.truncated = true;
    }

    const std::vector<std::pair<size_t, size_t>> spans =
        token_byte_spans(tokenize(body), body.size());
    if (spans.size() > text_budget) {
        size_t keep = text_budget;
        std::string mid;
        if (text_budget > elision_tokens + 1) {
            keep = text_budget - elision_tokens;
            mid = ELISION;
        }
        const size_t head = std::max<size_t>(1, keep / TEXT_HEAD_DIVISOR);
        const size_t tail = keep - head;
        const size_t head_end = head_end_for(spans, head, body);
        size_t tail_start = std::max(tail_start_for(spans, tail, body), head_end);
        if (head_end == 0 && tail_start == body.size()) {
            mid.clear();
            tail_start = tail_start_for(spans, text_budget, body);
        }
        body = body.substr(0, head_end) + mid + body.substr(tail_start);
        mark_from = head_end;
        mark_to = head_end + mid.size();
        fit.truncated = true;
    }

    // tokens(block + body) can differ from tokens(block) + tokens(body) where the
    // two meet, so the text is shortened by characters and tokenized again, as
    // ort-server does. The caller cuts ids only if this still does not fit.
    for (int attempt = 0;; ++attempt) {
        fit.input = build_router_input(labels, body);
        fit.input.marker_start = fit.input.text_start + mark_from;
        fit.input.marker_end = fit.input.text_start + mark_to;
        fit.tokens = tokenize(fit.input.text);
        const size_t n =
            token_byte_spans(fit.tokens, fit.input.text.size()).size() + n_specials;
        if (n <= window || attempt == MAX_REFIT_ATTEMPTS || body.empty()) break;
        const size_t chars = utf8_length(body);
        const size_t drop = std::min(chars, std::max<size_t>(1, n - window));
        body = body.substr(0, utf8_byte_at(body, chars - drop));
        fit.truncated = true;
    }
    return fit;
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
        const bool in_marker = start >= input.marker_start && end <= input.marker_end;
        if (end > input.text_start && !in_marker) groups.text.push_back(i);
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
        for (const size_t token : tokens) check_row_width(rows[token], head.hidden);
    };
    check_rows(groups.text);
    for (const auto& label : groups.labels) check_rows(label);

    std::vector<std::vector<double>> label_means;
    label_means.reserve(groups.labels.size());
    for (const auto& label : groups.labels) {
        label_means.push_back(mean_of_rows(rows, label, head.hidden));
    }
    return score_means(head, mean_of_rows(rows, groups.text, head.hidden), label_means);
}

RouterPoolGroups router_pool_groups(const RouterTokenGroups& groups) {
    auto runs = [](const std::vector<size_t>& tokens) {
        std::vector<std::pair<size_t, size_t>> out;
        for (const size_t token : tokens) {
            if (!out.empty() && out.back().second == token) {
                ++out.back().second;
            } else {
                out.emplace_back(token, token + 1);
            }
        }
        return out;
    };
    std::set<size_t> seen;
    auto disjoint = [&](const std::vector<size_t>& tokens) {
        for (const size_t token : tokens) {
            if (!seen.insert(token).second) return false;
        }
        return true;
    };
    RouterPoolGroups out;
    out.reserve(groups.labels.size() + 1);
    for (const auto& label : groups.labels) {
        if (!disjoint(label)) return {};
        out.push_back(runs(label));
    }
    if (!disjoint(groups.text)) return {};
    out.push_back(runs(groups.text));
    return out;
}

std::vector<double> score_pooled_router_labels(const RuleHead& head, const nlohmann::json& means,
                                               size_t n_labels) {
    if (!means.is_array() || means.size() != n_labels + 1) {
        throw std::runtime_error("llama-server returned " +
                                 std::to_string(means.is_array() ? means.size() : 0) +
                                 " pooled vectors for " + std::to_string(n_labels) +
                                 " labels and the text");
    }
    for (const auto& row : means) check_row_width(row, head.hidden);

    std::vector<std::vector<double>> label_means;
    label_means.reserve(n_labels);
    for (size_t i = 0; i < n_labels; ++i) label_means.push_back(row_of(means[i]));
    return score_means(head, row_of(means[n_labels]), label_means);
}

nlohmann::json router_classify_response(const std::vector<std::string>& labels,
                                        const std::vector<double>& probabilities, int top_k,
                                        bool truncated) {
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
    return nlohmann::json{{"labels", std::move(scores)}, {"truncated", truncated}};
}

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
