#pragma once

#include "lemon/server/smart_roles_selector.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace lemon::smart_roles {
using json = nlohmann::json;

struct Options {
    bool enabled = false;
    bool manual = false;
    std::string role;
    std::string model;
    State previous;
};

inline Options parse(const json& body) {
    Options opt;
    if (!body.is_object() || !body.contains("smart_roles")) return opt;
    const auto& value = body.at("smart_roles");
    if (!value.is_object()) throw std::invalid_argument("smart_roles must be an object");
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (it.key() != "mode" && it.key() != "decision_model" &&
            it.key() != "role" && it.key() != "state")
            throw std::invalid_argument("unknown smart_roles option: " + it.key());
    }
    const auto mode = value.value("mode", json("auto"));
    if (!mode.is_string() || (mode != "auto" && mode != "manual"))
        throw std::invalid_argument("smart_roles.mode must be auto or manual");
    opt.enabled = true;
    opt.manual = mode == "manual";
    if (value.contains("decision_model")) {
        if (!value["decision_model"].is_string()) throw std::invalid_argument("invalid decision_model");
        opt.model = value["decision_model"].get<std::string>();
        if (opt.model.size() > 200) throw std::invalid_argument("decision_model too long");
    }
    if (opt.manual) {
        if (!value.contains("role") || !value["role"].is_string())
            throw std::invalid_argument("manual smart_roles needs role");
        opt.role = value["role"].get<std::string>();
        if (!find(opt.role)) throw std::invalid_argument("unknown smart role");
    } else if (value.contains("role")) {
        throw std::invalid_argument("role is only supported with manual mode");
    }
    if (value.contains("state")) {
        if (!value["state"].is_object()) throw std::invalid_argument("invalid smart_roles.state");
        const auto& s = value["state"];
        if (s.size() > 2) throw std::invalid_argument("invalid smart_roles.state fields");
        if (s.contains("active")) {
            if (!s["active"].is_string()) throw std::invalid_argument("invalid smart_roles.state.active");
            opt.previous.active = s["active"].get<std::string>();
            if (!find(opt.previous.active)) throw std::invalid_argument("unknown active role");
        }
        if (s.contains("evidence")) {
            if (!s["evidence"].is_array() || s["evidence"].size() != kRoles.size())
                throw std::invalid_argument("invalid smart_roles.state.evidence");
            for (size_t i = 0; i < kRoles.size(); ++i) {
                const auto& v = s["evidence"][i];
                if (!v.is_number() || !std::isfinite(v.get<double>()) ||
                    v.get<double>() < 0 || v.get<double>() > 3)
                    throw std::invalid_argument("invalid smart roles evidence value");
                opt.previous.evidence[i] = v.get<double>();
            }
        }
    }
    return opt;
}

inline json state_json(const State& state) {
    json evidence = json::array();
    for (double v : state.evidence) evidence.push_back(std::round(v * 100) / 100);
    return {{"active", state.active}, {"evidence", evidence}};
}

inline std::string content_text(const json& content) {
    if (content.is_string()) return content.get<std::string>();
    std::string out;
    if (!content.is_array()) return out;
    for (const auto& piece : content) {
        if (piece.is_object() && piece.value("type", std::string{}) == "text" &&
            piece.contains("text") && piece["text"].is_string())
            out += piece["text"].get<std::string>() + "\n";
    }
    return out;
}

// Reject instructions inside fenced code and quoted lines; bound model cost.
inline std::string intent_text(const json& body, bool previous = false) {
    if (!body.contains("messages") || !body["messages"].is_array()) return {};
    int to_skip = previous ? 1 : 0;
    for (auto it = body["messages"].rbegin(); it != body["messages"].rend(); ++it) {
        if (!it->is_object() || it->value("role", std::string{}) != "user") continue;
        if (to_skip--) continue;
        std::string raw = content_text(it->value("content", json()));
        std::string result;
        bool code = false;
        size_t at = 0;
        while (at < raw.size() && result.size() < 1800) {
            const size_t end = raw.find('\n', at);
            const std::string line = raw.substr(at, end == std::string::npos ? end : end - at);
            const size_t first = line.find_first_not_of(" \t");
            if (first != std::string::npos && line.compare(first, 3, "```") == 0) code = !code;
            else if (!code && first != std::string::npos && line[first] != '>' &&
                     line.compare(first, 2, "//") != 0 && line.compare(first, 2, "# ") != 0)
                result += line.substr(0, std::min<size_t>(line.size(), 1800 - result.size())) + "\n";
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return result;
    }
    return {};
}

inline void inject(json& body, std::string_view role) {
    const auto* definition = find(role);
    if (!definition || definition->prompt.empty()) return;
    if (!body.contains("messages") || !body["messages"].is_array()) return;
    auto& messages = body["messages"];
    const std::string instruction = std::string("Task method (not a persona): ") +
        std::string(definition->prompt) +
        " Follow the caller's system rules and explicit user requirements. Do not claim unavailable tools.";
    if (!messages.empty() && messages[0].is_object() && messages[0].value("role", std::string{}) == "system" &&
        messages[0].value("content", json()).is_string()) {
        messages[0]["content"] = messages[0]["content"].get<std::string>() + "\n\n" + instruction;
    } else {
        messages.insert(messages.begin(), json{{"role", "system"}, {"content", instruction}});
    }
}

// d3 zero-shot classifiers return raw label scores. The selector decides a
// small positive-label menu; this wrapper validates the backend response and
// passes *all* scores to the pure, regression-tested normalizer.
inline Decision classify_decision(const json& result, const std::vector<std::string>& labels,
                                  const std::vector<size_t>& indices, const TaskSignal& signal) {
    Decision invalid;
    invalid.valid = false;
    if (!result.is_object() || !result.contains("labels") || !result["labels"].is_object() ||
        labels.size() != indices.size()) return invalid;
    std::vector<double> values;
    for (const auto& label : labels) {
        auto it = result["labels"].find(label);
        if (it == result["labels"].end() || !it->is_number()) return invalid;
        values.push_back(it->get<double>());
    }
    return score_decision(values, indices, signal);
}

inline Decision llm_decision(const json& response) {
    Decision d;
    try {
        const auto& answer = response.at("choices").at(0).at("message").at("content");
        if (!answer.is_string()) throw std::runtime_error("invalid content");
        const json j = json::parse(answer.get<std::string>());
        d.first.id = j.at("role").get<std::string>();
        d.first.score = j.at("score").get<double>();
        d.second.id = j.value("runner_up", "default");
        d.second.score = j.value("runner_up_score", 0.0);
        const std::string intent = j.value("intent_change", "none");
        const std::string scope = j.value("task_scope", "unclear");
        if (!find(d.first.id) || !find(d.second.id) ||
            (intent != "none" && intent != "implicit" && intent != "explicit") ||
            (scope != "continuation" && scope != "subtask" && scope != "new_task" && scope != "unclear"))
            throw std::runtime_error("invalid role or transition");
        d.intent = intent == "explicit" ? Intent::Explicit : intent == "implicit" ? Intent::Implicit : Intent::None;
        d.scope = scope == "continuation" ? Scope::Continuation : scope == "subtask" ? Scope::Subtask :
                  scope == "new_task" ? Scope::NewTask : Scope::Unclear;
    } catch (...) { d.valid = false; }
    return d;
}
} // namespace lemon::smart_roles
