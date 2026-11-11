#pragma once

#include "lemon/backends/llamacpp/llamacpp_rulehead.h"
#include "lemon/model_manager_types.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace lemon {
namespace backends {
namespace llamacpp {

struct DecisionTransport {
    std::function<nlohmann::json(const std::string&, const nlohmann::json&)> post;
    std::function<nlohmann::ordered_json(const std::string&, const nlohmann::ordered_json&)> post_ordered;
};

struct DecisionLaunch {
    bool rule_head = false;
    int context_size = 0;
    bool add_batch_size = false;
    bool add_ubatch_size = false;
    int batch_size = 0;
    int max_input_tokens = 0;
};

// The launch changes a SystemOne encoder or a rule-head router needs, or nullopt
// for every other model. Throws when the model names a rule head that is absent.
std::optional<DecisionLaunch> plan_decision_launch(const std::string& model_name, const ModelInfo& model_info,
                                                   int ctx_size, const std::string& custom_args);

class DecisionRuntime {
public:
    DecisionRuntime(const std::string& model_name, const ModelInfo& model_info);

    void set_max_input_tokens(int tokens) { encoder_max_input_tokens_ = tokens; }

    nlohmann::json classify(const DecisionTransport& transport, const nlohmann::json& request);
    nlohmann::ordered_json systemone(const DecisionTransport& transport, const nlohmann::ordered_json& request);

private:
    nlohmann::json unsupported_decision_request(const std::string& operation) const;
    nlohmann::ordered_json forward_systemone(const DecisionTransport& transport, const nlohmann::ordered_json& body);
    nlohmann::ordered_json decide(const DecisionTransport& transport, const nlohmann::ordered_json& body);
    std::optional<nlohmann::json> laya_cut_error(const DecisionTransport& transport, const nlohmann::ordered_json& body);
    nlohmann::json classify_with_rule_head(const DecisionTransport& transport, const nlohmann::json& request);

    std::string model_name_;
    std::shared_ptr<const RuleHead> rule_head_;
    std::mutex rule_head_counts_mutex_;
    std::optional<std::pair<size_t, size_t>> rule_head_counts_;
    bool decision_model_ = false;
    std::string decision_type_;
    int64_t decision_max_head_tokens_ = 0;
    int encoder_max_input_tokens_ = 0;
};

}  // namespace llamacpp
}  // namespace backends
}  // namespace lemon
