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

const char* to_string(SmartThinkingArithmeticOperationKind kind);

nlohmann::json smart_thinking_arithmetic_ir_to_json(
    const SmartThinkingArithmeticIR& ir, bool include_hash = true);

struct SmartThinkingArithmeticParseResult {
    bool parsed = false;
    SmartThinkingArithmeticIR ir;
    std::string failure_reason;
};

struct SmartThinkingArithmeticCompileResult {
    SmartThinkingCompileStatus status = SmartThinkingCompileStatus::NoMatch;
    SmartThinkingContractDetectionResult detection;
    std::optional<SmartThinkingArithmeticIR> ir;
    nlohmann::json compiled_task = nlohmann::json::object();
    std::string failure_reason;

    bool matched() const {
        return status != SmartThinkingCompileStatus::NoMatch;
    }

    bool compiled() const {
        return status == SmartThinkingCompileStatus::Compiled;
    }
};

class SmartThinkingArithmeticContractDetector {
public:
    static SmartThinkingContractDetectionResult detect(
        const std::string& task_text);
};

class SmartThinkingArithmeticParser {
public:
    static SmartThinkingArithmeticParseResult parse(
        const std::string& task_text);
};

class SmartThinkingArithmeticSemanticValidator {
public:
    static bool validate(const SmartThinkingArithmeticIR& ir,
                         std::string* failure_reason = nullptr);
};

class SmartThinkingArithmeticCompiler {
public:
    static SmartThinkingArithmeticCompileResult compile(
        const std::string& task_text);
};

class SmartThinkingArithmeticTransitionModel final
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

class SmartThinkingArithmeticExpansionPolicy final
    : public ISmartThinkingExpansionPolicy {
public:
    explicit SmartThinkingArithmeticExpansionPolicy(std::size_t chunk_size = 8);

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
    std::size_t chunk_size_ = 8;
};

class SmartThinkingArithmeticTerminalRenderer {
public:
    static std::optional<std::string> render(
        const nlohmann::json& compiled_task,
        const nlohmann::json& terminal_state,
        std::string* failure_reason = nullptr);
};

SmartThinkingCapabilityDescriptor smart_thinking_arithmetic_descriptor();
std::shared_ptr<const ISmartThinkingVerifiedCapability>
make_smart_thinking_arithmetic_capability();

}  // namespace lemon
