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
#include "lemon/smart_thinking_verified_ir.h"

namespace lemon {

nlohmann::json smart_thinking_constrained_selection_ir_to_json(
    const SmartThinkingConstrainedSelectionIR& ir,
    bool include_hash = true);

struct SmartThinkingConstrainedSelectionParseResult {
    bool parsed = false;
    SmartThinkingConstrainedSelectionIR ir;
    std::string failure_reason;
};

struct SmartThinkingConstrainedSelectionCompileResult {
    SmartThinkingCompileStatus status = SmartThinkingCompileStatus::NoMatch;
    SmartThinkingContractDetectionResult detection;
    std::optional<SmartThinkingConstrainedSelectionIR> ir;
    nlohmann::json compiled_task = nlohmann::json::object();
    std::string failure_reason;

    bool matched() const {
        return status != SmartThinkingCompileStatus::NoMatch;
    }

    bool compiled() const {
        return status == SmartThinkingCompileStatus::Compiled;
    }
};

class SmartThinkingConstrainedSelectionContractDetector {
public:
    static SmartThinkingContractDetectionResult detect(
        const std::string& task_text);
};

class SmartThinkingConstrainedSelectionParser {
public:
    static SmartThinkingConstrainedSelectionParseResult parse(
        const std::string& task_text);
};

class SmartThinkingConstrainedSelectionSemanticValidator {
public:
    static bool validate(const SmartThinkingConstrainedSelectionIR& ir,
                         std::string* failure_reason = nullptr);
};

class SmartThinkingConstrainedSelectionCompiler {
public:
    static SmartThinkingConstrainedSelectionCompileResult compile(
        const std::string& task_text);
};

SmartThinkingCapabilityDescriptor smart_thinking_constrained_selection_descriptor();
std::shared_ptr<const ISmartThinkingVerifiedCapability>
make_smart_thinking_constrained_selection_capability();

}  // namespace lemon
