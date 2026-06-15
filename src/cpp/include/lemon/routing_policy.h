#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

struct RoutingCandidate {
    std::string model;
    std::string description;
};

struct RoutingDecision {
    bool routed = false;
    bool fallback = false;
    std::string router_id;
    std::string router_type;
    std::string original_model;
    std::string selected_model;
    std::string rule_id;
    std::string reason;

    json to_json() const;
};

struct RoutingPolicy {
    std::string id;
    std::string type;
    std::string description;
    std::vector<std::string> endpoints;
    std::vector<RoutingCandidate> candidates;
    std::string default_model;
    int recommended_max_loaded_models = 1;
    json rules = json::array();

    // Agentic router settings. Ignored by heuristic routers.
    std::string router_model;
    std::string system_prompt;
    int max_decision_tokens = 128;
    double temperature = 0.0;
    std::string on_failure = "default";

    bool supports_endpoint(const std::string& endpoint) const;
    bool has_candidate(const std::string& model) const;
    json to_model_json() const;
    json to_json() const;
};

class RoutingPolicyEngine {
public:
    explicit RoutingPolicyEngine(const std::string& cache_dir);

    void reload_if_changed();
    bool has_router(const std::string& model_id);
    std::optional<RoutingPolicy> get_router(const std::string& model_id);
    std::vector<RoutingPolicy> routers();

    RoutingDecision route_heuristic(const RoutingPolicy& policy,
                                    const std::string& endpoint,
                                    const json& request) const;

private:
    std::filesystem::path routers_path_;
    std::filesystem::file_time_type last_loaded_mtime_{};
    bool loaded_once_ = false;
    std::vector<RoutingPolicy> routers_;
    mutable std::mutex mutex_;

    void reload_if_changed_locked();
    void load();
    static RoutingPolicy parse_policy(const json& item);
};

std::string routing_request_text(const std::string& endpoint, const json& request);
bool routing_request_has_images(const json& request);
std::string extract_json_object_text(const std::string& text);

} // namespace lemon
