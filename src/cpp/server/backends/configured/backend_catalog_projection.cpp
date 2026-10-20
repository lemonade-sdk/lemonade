#include "backend_catalog_admission_internal.h"

#include <algorithm>
#include <cctype>
#include <set>
#include "lemon/backends/configured/backend_catalog_admission.h"

namespace lemon::backends {

nlohmann::json public_admission_diagnostic(const CompositionDiagnostic& diagnostic) {
    static const auto fields = [] {
        std::set<std::string> names;
        const auto collect = [&](const auto& self, const nlohmann::json& value) -> void {
            if (value.is_object()) for (const auto& [key, child] : value.items()) {
                if (key == "properties" && child.is_object()) for (const auto& [name, property] : child.items()) names.insert(name);
                self(self, child);
            } else if (value.is_array()) for (const auto& child : value) self(self, child);
        };
        collect(collect, v2::schema_document());
        return names;
    }();
    std::string pointer;
    if (diagnostic.pointer.size() <= DefinitionLimits{}.pointer_bytes) {
        const auto tokens = v2::pointer_tokens(diagnostic.pointer, DefinitionLimits{});
        for (const auto& token : tokens) {
            const bool index = !token.empty() && std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); });
            if (!index && !fields.count(token)) break;
            pointer += "/" + v2::pointer_token(token);
        }
    }
    const auto& id = diagnostic.definition;
    const bool logical = !id.empty() && id.size() <= 128 && id[0] >= 'a' && id[0] <= 'z' &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'; });
    return {{"code", diagnostic.code}, {"definition", logical ? id : ""}, {"pointer", pointer}, {"message", "Definition admission failed"}};
}

nlohmann::json installed_definition_admission_features() {
    const DefinitionLimits limits;
    const auto& schema = v2::schema_document();
    return {{"manifest_version", 1}, {"definition_schema_version", 2},
        {"schema_id", schema_digest(schema.dump())}, {"accepted_definition_versions", {1, 2}},
        {"acceptance_stage", "declaration_authorization"}, {"executable_definition_versions", {1}},
        {"resolver_revision", 1}, {"authorizer_revision", 1}, {"grant_policy_revision", 1},
        {"export_registry_id", installed_builtin_exports().digest()},
        {"hook_abi_id", v2::identity("lemonade.backend-hook-abi.v1\n", v2::hook_abi_json(installed_hook_abis()))},
        {"bounds", {{"definitions", limits.snapshot_documents}, {"source_bytes", limits.snapshot_source_bytes},
            {"retained_logical_bytes", limits.retained_logical_bytes}, {"recipe_rows", 1024}, {"diagnostics", limits.diagnostics}}}};
}

}
