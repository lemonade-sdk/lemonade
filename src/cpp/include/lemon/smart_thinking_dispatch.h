#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lemon/smart_thinking_capability.h"
#include "lemon/smart_thinking_search.h"
#include "lemon/smart_thinking_verified_ir.h"

namespace lemon {

nlohmann::json smart_thinking_dispatch_ir_to_json(
    const SmartThinkingDispatchIR& ir, bool include_hash = true);

struct SmartThinkingDispatchParseResult {
    bool parsed = false;
    SmartThinkingDispatchIR ir;
    std::string failure_reason;
};

struct SmartThinkingDispatchCompileResult {
    SmartThinkingCompileStatus status = SmartThinkingCompileStatus::NoMatch;
    SmartThinkingContractDetectionResult detection;
    std::optional<SmartThinkingDispatchIR> ir;
    nlohmann::json compiled_task = nlohmann::json::object();
    std::string failure_reason;

    bool matched() const {
        return status != SmartThinkingCompileStatus::NoMatch;
    }

    bool compiled() const {
        return status == SmartThinkingCompileStatus::Compiled;
    }
};

class SmartThinkingDispatchContractDetector {
public:
    static SmartThinkingContractDetectionResult detect(
        const std::string& task_text);
};

class SmartThinkingDispatchParser {
public:
    static SmartThinkingDispatchParseResult parse(
        const std::string& task_text);
};

class SmartThinkingDispatchSemanticValidator {
public:
    static bool validate(const SmartThinkingDispatchIR& ir,
                         std::string* failure_reason = nullptr);
};

class SmartThinkingDispatchCompiler {
public:
    static SmartThinkingDispatchCompileResult compile(
        const std::string& task_text);
};

class SmartThinkingDispatchTransitionModel final
    : public ISmartThinkingTransitionModel {
public:
    nlohmann::json initial_state(
        const nlohmann::json& compiled_task) const override;

    SmartThinkingTransitionResult step(
        const nlohmann::json& compiled_task,
        const nlohmann::json& state,
        const nlohmann::json& action) const override;

    bool is_terminal(
        const nlohmann::json& compiled_task,
        const nlohmann::json& state) const override;
};

class SmartThinkingDispatchExpansionPolicy final
    : public ISmartThinkingExpansionPolicy {
public:
    explicit SmartThinkingDispatchExpansionPolicy(std::size_t chunk_size = 4);

    bool consumes_model_call() const override {
        return false;
    }

    std::vector<nlohmann::json> propose_actions(
        const nlohmann::json& compiled_task,
        const nlohmann::json& state,
        const SmartThinkingSearchBudget& budget) override;

    std::size_t chunk_size() const {
        return chunk_size_;
    }

private:
    std::size_t chunk_size_ = 4;
};

class SmartThinkingDispatchTerminalRenderer {
public:
    static std::optional<std::string> render(
        const nlohmann::json& compiled_task,
        const nlohmann::json& terminal_state,
        std::string* failure_reason = nullptr);
};

SmartThinkingCapabilityDescriptor smart_thinking_dispatch_descriptor();
std::shared_ptr<const ISmartThinkingVerifiedCapability>
make_smart_thinking_dispatch_capability();

}  // namespace lemon
