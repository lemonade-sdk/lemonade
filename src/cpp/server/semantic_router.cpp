#include "lemon/semantic_router.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace lemon {

SemanticRouter::SemanticRouter(const std::string& cache_dir)
    : cache_dir_(cache_dir), enabled_(false) {
}

std::string SemanticRouter::get_config_path() const {
    return (fs::path(cache_dir_) / "semantic_routing.yaml").string();
}

bool SemanticRouter::load_config(const std::string& config_path) {
    try {
        std::ifstream file(config_path);
        if (!file.is_open()) {
            LOG(DEBUG, "SemanticRouter") << "No config file at: " << config_path << std::endl;
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        config_yaml_ = buffer.str();

        // Parse YAML to extract enabled flag and keyword rules
        // For now, do simple line-by-line parsing (proper YAML parser would be better)
        enabled_ = config_yaml_.find("enabled: true") != std::string::npos;

        parse_keyword_rules();

        LOG(INFO, "SemanticRouter") << "Loaded config from: " << config_path
            << " (enabled: " << (enabled_ ? "yes" : "no") << ")" << std::endl;
        return true;
    } catch (const std::exception& e) {
        LOG(ERROR, "SemanticRouter") << "Failed to load config: " << e.what() << std::endl;
        return false;
    }
}

bool SemanticRouter::save_config(const std::string& config_yaml) {
    try {
        config_yaml_ = config_yaml;

        // Save to file
        std::string config_path = get_config_path();
        fs::path parent = fs::path(config_path).parent_path();
        if (!fs::exists(parent)) {
            fs::create_directories(parent);
        }

        std::ofstream file(config_path);
        if (!file.is_open()) {
            LOG(ERROR, "SemanticRouter") << "Failed to open config file for writing: " << config_path << std::endl;
            return false;
        }

        file << config_yaml;
        file.close();

        // Re-parse
        enabled_ = config_yaml_.find("enabled: true") != std::string::npos;
        parse_keyword_rules();

        LOG(INFO, "SemanticRouter") << "Saved config to: " << config_path << std::endl;
        return true;
    } catch (const std::exception& e) {
        LOG(ERROR, "SemanticRouter") << "Failed to save config: " << e.what() << std::endl;
        return false;
    }
}

std::string SemanticRouter::get_config_yaml() const {
    return config_yaml_;
}

bool SemanticRouter::is_enabled() const {
    return enabled_;
}

void SemanticRouter::parse_keyword_rules() {
    keyword_rules_.clear();

    // Simple parser for keywords section
    // Format:
    //   keywords:
    //     rule_name:
    //       corpus:
    //         - "keyword1"
    //         - "keyword2"
    //       threshold: 0.25
    //       target: "model-name"

    std::istringstream stream(config_yaml_);
    std::string line;
    bool in_keywords = false;
    std::string current_rule;
    KeywordRule rule;
    bool in_corpus = false;

    while (std::getline(stream, line)) {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

        if (trimmed == "keywords:") {
            in_keywords = true;
            continue;
        }

        if (!in_keywords) continue;

        // End of keywords section
        if (trimmed.empty() || (trimmed[0] != ' ' && trimmed.find(':') != std::string::npos && trimmed.find("corpus") == std::string::npos && trimmed.find("threshold") == std::string::npos && trimmed.find("target") == std::string::npos)) {
            if (!current_rule.empty() && !rule.corpus.empty()) {
                keyword_rules_.push_back(rule);
            }
            in_keywords = false;
            break;
        }

        // New rule name (e.g., "  simple_query:")
        if (trimmed.find(':') != std::string::npos && trimmed.find("corpus") == std::string::npos && trimmed.find("threshold") == std::string::npos && trimmed.find("target") == std::string::npos) {
            // Save previous rule
            if (!current_rule.empty() && !rule.corpus.empty()) {
                keyword_rules_.push_back(rule);
            }

            current_rule = trimmed.substr(0, trimmed.find(':'));
            rule = KeywordRule();
            rule.name = current_rule;
            in_corpus = false;
        }
        else if (trimmed == "corpus:") {
            in_corpus = true;
        }
        else if (in_corpus && trimmed.find("- ") == 0) {
            // Extract corpus item (remove "- " and quotes)
            std::string item = trimmed.substr(2);
            if (!item.empty() && item.front() == '"') item = item.substr(1);
            if (!item.empty() && item.back() == '"') item.pop_back();
            if (!item.empty()) {
                rule.corpus.push_back(item);
            }
        }
        else if (trimmed.find("threshold:") == 0) {
            in_corpus = false;
            try {
                rule.threshold = std::stof(trimmed.substr(trimmed.find(':') + 1));
            } catch (...) {
                rule.threshold = 0.25f;
            }
        }
        else if (trimmed.find("target:") == 0) {
            in_corpus = false;
            std::string target = trimmed.substr(trimmed.find(':') + 1);
            target.erase(0, target.find_first_not_of(" \t\""));
            target.erase(target.find_last_not_of(" \t\"\r\n") + 1);
            rule.target_model = target;
        }
    }

    // Save last rule
    if (!current_rule.empty() && !rule.corpus.empty()) {
        keyword_rules_.push_back(rule);
    }

    LOG(DEBUG, "SemanticRouter") << "Parsed " << keyword_rules_.size() << " keyword rules" << std::endl;
}

json SemanticRouter::route_by_bm25(const std::string& prompt) {
    if (keyword_rules_.empty()) {
        return json::object();
    }

    // Simple BM25-like scoring
    // Split prompt into tokens
    std::vector<std::string> query_tokens;
    std::istringstream iss(prompt);
    std::string token;
    while (iss >> token) {
        // Convert to lowercase
        std::transform(token.begin(), token.end(), token.begin(), ::tolower);
        query_tokens.push_back(token);
    }

    if (query_tokens.empty()) {
        return json::object();
    }

    // Check each rule
    for (const auto& rule : keyword_rules_) {
        for (const auto& keyword : rule.corpus) {
            // Simple substring match (case-insensitive)
            std::string lower_prompt = prompt;
            std::transform(lower_prompt.begin(), lower_prompt.end(), lower_prompt.begin(), ::tolower);
            std::string lower_keyword = keyword;
            std::transform(lower_keyword.begin(), lower_keyword.end(), lower_keyword.begin(), ::tolower);

            if (lower_prompt.find(lower_keyword) != std::string::npos) {
                // Match found
                LOG(DEBUG, "SemanticRouter") << "BM25 match: '" << keyword
                    << "' → " << rule.target_model << std::endl;

                return {
                    {"action", "redirect"},
                    {"model", rule.target_model},
                    {"reason", "Keyword match: '" + keyword + "'"},
                    {"matched_by", "bm25"},
                    {"latency_ms", 0}
                };
            }
        }
    }

    return json::object();
}

json SemanticRouter::route_by_ml(const std::string& prompt, int python_service_port) {
    try {
        std::string url = "http://127.0.0.1:" + std::to_string(python_service_port) + "/route";
        json request_body = {{"prompt", prompt}};

        std::map<std::string, std::string> headers;
        headers["Content-Type"] = "application/json";

        auto result = lemon::utils::HttpClient::post(url, request_body.dump(), headers, 2);

        if (result.status_code != 200) {
            LOG(DEBUG, "SemanticRouter") << "ML service returned status " << result.status_code << std::endl;
            return json::object();
        }

        auto response = json::parse(result.body);
        response["matched_by"] = "ml";
        return response;
    } catch (const std::exception& e) {
        LOG(DEBUG, "SemanticRouter") << "ML routing error: " << e.what() << std::endl;
        return json::object();
    }
}

json SemanticRouter::route(const std::string& prompt, int python_service_port) {
    if (!enabled_) {
        return {
            {"action", "allow"},
            {"model", nullptr},
            {"reason", "Routing disabled"}
        };
    }

    // Fast path: BM25 keyword matching (C++)
    auto bm25_result = route_by_bm25(prompt);
    if (!bm25_result.empty() && bm25_result.value("action", "") != "") {
        return bm25_result;
    }

    // Slow path: ML signals (Python service)
    auto ml_result = route_by_ml(prompt, python_service_port);
    if (!ml_result.empty() && ml_result.value("action", "") != "") {
        return ml_result;
    }

    // No match
    return {
        {"action", "allow"},
        {"model", nullptr},
        {"reason", "No routing rules matched"},
        {"latency_ms", 0}
    };
}

} // namespace lemon
