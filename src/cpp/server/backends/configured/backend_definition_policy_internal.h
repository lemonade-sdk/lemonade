#pragma once

#include "backend_definition_exports_internal.h"

namespace lemon::backends::v2 {

Json fragment_rules_json(const std::vector<MutableFragmentRule>& rules);
void audit_fragment_rules(const std::vector<MutableFragmentRule>& rules, const Json& declaration, Budget& budget);
void verify_fragment_envelope(const BuiltinExportContract& contract, const Json& declaration, Budget& budget);
void validate_fragments(const std::vector<MutableFragmentRule>& rules, const Json& declaration,
    const Json& base, Budget& budget);
void mask_fragments(Json& declaration, const std::vector<MutableFragmentRule>& rules, Budget& budget);
void inspect_fragment_write(const std::vector<MutableFragmentRule>& rules, const NodePtr& root,
    const std::string& pointer, DefinitionEditKind kind, const Json* value,
    const SourceLocation& source, Budget& budget);
struct TypedReferenceMetadata {
    MutableValueType type;
    ReferencePhase phase;
};

std::optional<TypedReferenceMetadata> typed_data_reference(const std::string& path, const Json& declaration);

}
