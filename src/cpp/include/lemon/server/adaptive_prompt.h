#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon::adaptive_prompt {

using json = nlohmann::json;

struct Module {
    std::string id;
    std::string type; // "role" (exclusive) or "skill" (combinable)
    std::string group; // optional exclusivity group, also for skills
    std::string description;
    std::string prompt;
    std::vector<std::string> keywords;
    std::vector<std::string> examples;
    std::vector<std::string> excludes;
    int priority = 0;
};

enum class Mode { Off, Rules, Llm, Manual };

struct Options {
    Mode mode = Mode::Off;
    std::vector<std::string> ids;
    std::vector<std::string> previous_ids; // caller-carried state for this conversation
    std::string decision_model; // optional, downloaded helpers are loaded on demand
    bool trace = false;
};

Options parse_options(const json& option);
std::vector<Module> parse_catalog(const json& doc);
std::vector<Module> builtin_catalog();
const std::vector<Module>& catalog();
std::string lowercase_utf8(std::string text);
bool phrase_match(const std::string& text, const std::string& phrase);
std::string last_user_text(const json& body);
std::string previous_user_text(const json& body);
std::string intent_excerpt(const std::string& text);
std::vector<std::string> select_ids(const std::string& user_text,
                                    const std::vector<Module>& modules);
std::vector<std::string> constrain_ids(const std::vector<std::string>& proposed,
                                       const std::vector<Module>& modules);
std::vector<std::string> merge_ids(const std::vector<std::string>& previous,
                                   const std::vector<std::string>& current,
                                   const std::vector<Module>& modules);
std::vector<std::string> decide_with_model(
    const std::string& text, const std::vector<Module>& modules,
    const std::string& model, const std::function<json(const json&)>& complete,
    const std::string& previous_user = {});
std::vector<std::string> decide_with_classifier(
    const std::string& text, const std::vector<Module>& modules,
    const std::string& model, const std::function<json(const json&)>& classify,
    const std::string& previous_user = {});
std::vector<std::string> apply_ids(json& body, const std::vector<Module>& modules,
                                   const std::vector<std::string>& proposed);
std::string join_ids(const std::vector<std::string>& ids);

} // namespace lemon::adaptive_prompt
