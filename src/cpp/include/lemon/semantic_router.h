#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

/**
 * Semantic Router - Intelligent prompt-based routing
 *
 * Routes requests between local and cloud models based on:
 * - BM25 keyword matching (C++ - fast)
 * - ML signals via Python service (jailbreak, PII, complexity)
 */
class SemanticRouter {
public:
    SemanticRouter(const std::string& cache_dir);
    ~SemanticRouter() = default;

    /**
     * Load config from YAML file
     * @param config_path Path to semantic_routing.yaml
     */
    bool load_config(const std::string& config_path);

    /**
     * Save config to YAML file
     * @param config_yaml YAML content as string
     */
    bool save_config(const std::string& config_yaml);

    /**
     * Get current config as YAML string
     */
    std::string get_config_yaml() const;

    /**
     * Get config file path
     */
    std::string get_config_path() const;

    /**
     * Route a prompt to determine target model
     *
     * Fast path: Check BM25 keywords locally
     * Slow path: Call Python service for ML signals
     *
     * @param prompt User's message
     * @param python_service_port Port for Python ML service
     * @return Routing decision {action, model, reason}
     */
    json route(const std::string& prompt, int python_service_port = 8765);

    /**
     * Check if routing is enabled
     */
    bool is_enabled() const;

private:
    struct KeywordRule {
        std::string name;
        std::vector<std::string> corpus;
        float threshold;
        std::string target_model;
    };

    /**
     * BM25 keyword matching (C++ implementation)
     */
    json route_by_bm25(const std::string& prompt);

    /**
     * Call Python service for ML-based routing
     */
    json route_by_ml(const std::string& prompt, int python_service_port);

    /**
     * Parse config and extract keyword rules
     */
    void parse_keyword_rules();

    std::string cache_dir_;
    std::string config_yaml_;
    json config_;
    bool enabled_ = false;
    std::vector<KeywordRule> keyword_rules_;
};

} // namespace lemon
