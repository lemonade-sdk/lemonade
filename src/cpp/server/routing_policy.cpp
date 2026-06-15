#include "lemon/routing_policy.h"
#include "lemon/utils/json_utils.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <lemon/utils/aixlog.hpp>

namespace fs = std::filesystem;

namespace lemon {
namespace {

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::vector<std::string> json_string_array(const json& value, const std::string& field) {
    std::vector<std::string> out;
    if (!value.contains(field)) return out;
    if (!value[field].is_array()) {
        throw std::runtime_error("Router field '" + field + "' must be an array");
    }
    for (const auto& item : value[field]) {
        if (!item.is_string()) {
            throw std::runtime_error("Router field '" + field + "' must contain only strings");
        }
        out.push_back(item.get<std::string>());
    }
    return out;
}

void append_content_text(std::ostringstream& out, const json& content) {
    if (content.is_string()) {
        out << content.get<std::string>() << "\n";
        return;
    }

    if (!content.is_array()) {
        return;
    }

    for (const auto& part : content) {
        if (part.is_string()) {
            out << part.get<std::string>() << "\n";
            continue;
        }
        if (!part.is_object()) {
            continue;
        }
        const std::string type = part.value("type", "");
        if ((type == "text" || type == "input_text") && part.contains("text") && part["text"].is_string()) {
            out << part["text"].get<std::string>() << "\n";
        }
    }
}

bool json_contains_image_part(const json& value) {
    if (value.is_object()) {
        const std::string type = value.value("type", "");
        if (type == "image_url" || type == "input_image") return true;
        if (value.contains("image_url") || value.contains("input_image")) return true;
        for (const auto& item : value.items()) {
            if (json_contains_image_part(item.value())) return true;
        }
        return false;
    }
    if (value.is_array()) {
        for (const auto& item : value) {
            if (json_contains_image_part(item)) return true;
        }
    }
    return false;
}

bool keywords_any_match(const json& matcher, const std::string& text_lower) {
    if (!matcher.contains("keywords_any")) return true;
    if (!matcher["keywords_any"].is_array()) return false;
    for (const auto& keyword : matcher["keywords_any"]) {
        if (keyword.is_string() && text_lower.find(to_lower(keyword.get<std::string>())) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool keywords_all_match(const json& matcher, const std::string& text_lower) {
    if (!matcher.contains("keywords_all")) return true;
    if (!matcher["keywords_all"].is_array()) return false;
    for (const auto& keyword : matcher["keywords_all"]) {
        if (!keyword.is_string()) return false;
        if (text_lower.find(to_lower(keyword.get<std::string>())) == std::string::npos) {
            return false;
        }
    }
    return true;
}

bool regex_matcher(const json& matcher, const std::string& text) {
    if (!matcher.contains("regex")) return true;
    if (!matcher["regex"].is_string()) return false;
    try {
        std::regex re(matcher["regex"].get<std::string>(),
                      std::regex::ECMAScript | std::regex::icase);
        return std::regex_search(text, re);
    } catch (const std::regex_error& e) {
        LOG(WARNING, "Routing") << "Invalid router regex: " << e.what() << std::endl;
        return false;
    }
}

bool match_condition(const json& matcher, const std::string& text, const json& request) {
    if (!matcher.is_object()) {
        return false;
    }

    if (matcher.contains("any")) {
        if (!matcher["any"].is_array()) return false;
        for (const auto& item : matcher["any"]) {
            if (match_condition(item, text, request)) return true;
        }
        return false;
    }

    if (matcher.contains("all")) {
        if (!matcher["all"].is_array()) return false;
        for (const auto& item : matcher["all"]) {
            if (!match_condition(item, text, request)) return false;
        }
        return true;
    }

    if (matcher.contains("not")) {
        return !match_condition(matcher["not"], text, request);
    }

    const std::string text_lower = to_lower(text);

    if (!keywords_any_match(matcher, text_lower)) return false;
    if (!keywords_all_match(matcher, text_lower)) return false;
    if (!regex_matcher(matcher, text)) return false;

    if (matcher.contains("min_chars")) {
        if (!matcher["min_chars"].is_number_unsigned() && !matcher["min_chars"].is_number_integer()) return false;
        if (text.size() < matcher["min_chars"].get<size_t>()) return false;
    }
    if (matcher.contains("max_chars")) {
        if (!matcher["max_chars"].is_number_unsigned() && !matcher["max_chars"].is_number_integer()) return false;
        if (text.size() > matcher["max_chars"].get<size_t>()) return false;
    }
    if (matcher.contains("has_tools")) {
        if (!matcher["has_tools"].is_boolean()) return false;
        const bool has_tools = request.contains("tools") && request["tools"].is_array() && !request["tools"].empty();
        if (has_tools != matcher["has_tools"].get<bool>()) return false;
    }
    if (matcher.contains("has_images")) {
        if (!matcher["has_images"].is_boolean()) return false;
        if (routing_request_has_images(request) != matcher["has_images"].get<bool>()) return false;
    }

    return true;
}

std::vector<RoutingCandidate> parse_candidates(const json& value) {
    std::vector<RoutingCandidate> out;
    if (!value.contains("candidates")) return out;
    if (!value["candidates"].is_array()) {
        throw std::runtime_error("Router field 'candidates' must be an array");
    }
    for (const auto& item : value["candidates"]) {
        RoutingCandidate candidate;
        if (item.is_string()) {
            candidate.model = item.get<std::string>();
        } else if (item.is_object()) {
            candidate.model = item.value("model", "");
            candidate.description = item.value("description", "");
        }
        if (candidate.model.empty()) {
            throw std::runtime_error("Router candidates must name a model");
        }
        out.push_back(candidate);
    }
    return out;
}

} // namespace

json RoutingDecision::to_json() const {
    return {
        {"routed", routed},
        {"router", router_id},
        {"type", router_type},
        {"original_model", original_model},
        {"selected_model", selected_model},
        {"rule", rule_id},
        {"reason", reason}
    };
}

bool RoutingPolicy::supports_endpoint(const std::string& endpoint) const {
    if (endpoints.empty()) return true;
    return std::find(endpoints.begin(), endpoints.end(), endpoint) != endpoints.end();
}

bool RoutingPolicy::has_candidate(const std::string& model) const {
    if (candidates.empty()) return true;
    return std::any_of(candidates.begin(), candidates.end(),
                       [&model](const RoutingCandidate& candidate) {
                           return candidate.model == model;
                       });
}

json RoutingPolicy::to_model_json() const {
    json labels = json::array({"router", type});
    return {
        {"id", id},
        {"object", "model"},
        {"created", 1234567890},
        {"owned_by", "lemonade"},
        {"checkpoint", ""},
        {"checkpoints", json::object()},
        {"recipe", "router"},
        {"downloaded", true},
        {"suggested", false},
        {"labels", labels},
        {"components", json::array()},
        {"recipe_options", json::object()},
        {"router", to_json()}
    };
}

json RoutingPolicy::to_json() const {
    json candidate_json = json::array();
    for (const auto& candidate : candidates) {
        json item = {{"model", candidate.model}};
        if (!candidate.description.empty()) item["description"] = candidate.description;
        candidate_json.push_back(item);
    }

    json result = {
        {"id", id},
        {"type", type},
        {"description", description},
        {"endpoints", endpoints},
        {"default_model", default_model},
        {"recommended_max_loaded_models", recommended_max_loaded_models},
        {"candidates", candidate_json}
    };
    if (type == "agentic") {
        result["router_model"] = router_model;
        result["max_decision_tokens"] = max_decision_tokens;
        result["temperature"] = temperature;
        result["on_failure"] = on_failure;
    }
    return result;
}

RoutingPolicyEngine::RoutingPolicyEngine(const std::string& cache_dir)
    : routers_path_(fs::path(cache_dir) / "routers.json") {
    load();
}

void RoutingPolicyEngine::reload_if_changed() {
    std::lock_guard<std::mutex> lock(mutex_);
    reload_if_changed_locked();
}

void RoutingPolicyEngine::reload_if_changed_locked() {
    std::error_code ec;
    if (!fs::exists(routers_path_, ec)) {
        if (loaded_once_ && !routers_.empty()) {
            LOG(INFO, "Routing") << "routers.json removed; clearing routing policies" << std::endl;
            routers_.clear();
        }
        loaded_once_ = true;
        return;
    }

    auto current_mtime = fs::last_write_time(routers_path_, ec);
    if (ec) {
        LOG(WARNING, "Routing") << "Unable to stat routers.json: " << ec.message() << std::endl;
        return;
    }

    if (!loaded_once_ || current_mtime != last_loaded_mtime_) {
        load();
    }
}

bool RoutingPolicyEngine::has_router(const std::string& model_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    reload_if_changed_locked();
    return std::any_of(routers_.begin(), routers_.end(),
                       [&model_id](const RoutingPolicy& policy) {
                           return policy.id == model_id;
                       });
}

std::optional<RoutingPolicy> RoutingPolicyEngine::get_router(const std::string& model_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    reload_if_changed_locked();
    for (const auto& policy : routers_) {
        if (policy.id == model_id) return policy;
    }
    return std::nullopt;
}

std::vector<RoutingPolicy> RoutingPolicyEngine::routers() {
    std::lock_guard<std::mutex> lock(mutex_);
    reload_if_changed_locked();
    return routers_;
}

void RoutingPolicyEngine::load() {
    std::error_code ec;
    loaded_once_ = true;
    if (!fs::exists(routers_path_, ec)) {
        routers_.clear();
        return;
    }

    try {
        json root = utils::JsonUtils::load_from_file(routers_path_.string());
        if (!root.is_object()) {
            throw std::runtime_error("routers.json root must be an object");
        }
        if (!root.contains("routers") || !root["routers"].is_array()) {
            throw std::runtime_error("routers.json must contain a 'routers' array");
        }

        std::vector<RoutingPolicy> loaded;
        std::unordered_set<std::string> ids;
        for (const auto& item : root["routers"]) {
            RoutingPolicy policy = parse_policy(item);
            if (!ids.insert(policy.id).second) {
                throw std::runtime_error("Duplicate router id: " + policy.id);
            }
            loaded.push_back(std::move(policy));
        }

        routers_ = std::move(loaded);
        last_loaded_mtime_ = fs::last_write_time(routers_path_, ec);
        LOG(INFO, "Routing") << "Loaded " << routers_.size()
                             << " routing polic" << (routers_.size() == 1 ? "y" : "ies")
                             << " from " << routers_path_.string() << std::endl;
    } catch (const std::exception& e) {
        LOG(WARNING, "Routing") << "Failed to load routers.json: " << e.what() << std::endl;
        routers_.clear();
    }
}

RoutingPolicy RoutingPolicyEngine::parse_policy(const json& item) {
    if (!item.is_object()) {
        throw std::runtime_error("Each router entry must be an object");
    }

    RoutingPolicy policy;
    policy.id = item.value("id", "");
    policy.type = item.value("type", "");
    policy.description = item.value("description", "");
    policy.default_model = item.value("default_model", "");
    policy.recommended_max_loaded_models = item.value(
        "recommended_max_loaded_models",
        policy.type == "agentic" ? 2 : 1);
    policy.endpoints = json_string_array(item, "endpoints");
    policy.candidates = parse_candidates(item);

    if (policy.id.empty()) throw std::runtime_error("Router is missing id");
    if (policy.type != "heuristic" && policy.type != "agentic") {
        throw std::runtime_error("Router '" + policy.id + "' has unsupported type: " + policy.type);
    }
    if (policy.default_model.empty()) {
        throw std::runtime_error("Router '" + policy.id + "' is missing default_model");
    }
    if (policy.recommended_max_loaded_models < 1) {
        throw std::runtime_error("Router '" + policy.id + "' recommended_max_loaded_models must be positive");
    }
    if (!policy.has_candidate(policy.default_model)) {
        throw std::runtime_error("Router '" + policy.id + "' default_model is not in candidates");
    }

    if (item.contains("rules")) {
        if (!item["rules"].is_array()) {
            throw std::runtime_error("Router '" + policy.id + "' rules must be an array");
        }
        policy.rules = item["rules"];
    }

    if (policy.type == "agentic") {
        policy.router_model = item.value("router_model", "");
        policy.system_prompt = item.value("system_prompt", "");
        policy.max_decision_tokens = item.value("max_decision_tokens", 128);
        policy.temperature = item.value("temperature", 0.0);
        policy.on_failure = item.value("on_failure", "default");
        if (policy.router_model.empty()) {
            throw std::runtime_error("Agentic router '" + policy.id + "' is missing router_model");
        }
        if (policy.router_model == policy.id) {
            throw std::runtime_error("Agentic router '" + policy.id + "' cannot use itself as router_model");
        }
        if (policy.on_failure != "default" && policy.on_failure != "error") {
            throw std::runtime_error("Agentic router '" + policy.id + "' on_failure must be default or error");
        }
    }

    return policy;
}

RoutingDecision RoutingPolicyEngine::route_heuristic(const RoutingPolicy& policy,
                                                     const std::string& endpoint,
                                                     const json& request) const {
    RoutingDecision decision;
    decision.routed = true;
    decision.router_id = policy.id;
    decision.router_type = policy.type;
    decision.original_model = request.value("model", "");
    decision.selected_model = policy.default_model;
    decision.reason = "default_model";

    if (!policy.supports_endpoint(endpoint)) {
        decision.selected_model.clear();
        decision.reason = "router does not support endpoint: " + endpoint;
        return decision;
    }

    const std::string text = routing_request_text(endpoint, request);
    for (const auto& rule : policy.rules) {
        if (!rule.is_object()) continue;
        const std::string route_to = rule.value("route_to", "");
        if (route_to.empty()) continue;
        if (!policy.has_candidate(route_to)) {
            LOG(WARNING, "Routing") << "Rule route_to is not in candidates for router "
                                    << policy.id << ": " << route_to << std::endl;
            continue;
        }
        const json matcher = rule.contains("match") ? rule["match"] : json::object();
        if (match_condition(matcher, text, request)) {
            decision.selected_model = route_to;
            decision.rule_id = rule.value("id", "");
            decision.reason = decision.rule_id.empty()
                ? "matched heuristic rule"
                : "matched heuristic rule: " + decision.rule_id;
            return decision;
        }
    }

    return decision;
}

std::string routing_request_text(const std::string& endpoint, const json& request) {
    std::ostringstream out;
    if (endpoint == "chat.completions" && request.contains("messages") && request["messages"].is_array()) {
        for (const auto& message : request["messages"]) {
            if (!message.is_object()) continue;
            if (message.contains("role") && message["role"].is_string()) {
                out << message["role"].get<std::string>() << ": ";
            }
            if (message.contains("content")) {
                append_content_text(out, message["content"]);
            }
        }
        return out.str();
    }

    if (endpoint == "completions" && request.contains("prompt")) {
        const auto& prompt = request["prompt"];
        if (prompt.is_string()) return prompt.get<std::string>();
        if (prompt.is_array()) {
            for (const auto& item : prompt) {
                if (item.is_string()) out << item.get<std::string>() << "\n";
            }
            return out.str();
        }
    }

    if (endpoint == "responses" && request.contains("input")) {
        const auto& input = request["input"];
        if (input.is_string()) return input.get<std::string>();
        if (input.is_array() || input.is_object()) return input.dump();
    }

    return request.dump();
}

bool routing_request_has_images(const json& request) {
    return json_contains_image_part(request);
}

std::string extract_json_object_text(const std::string& text) {
    size_t start = text.find('{');
    size_t end = text.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        return "";
    }
    return text.substr(start, end - start + 1);
}

} // namespace lemon
