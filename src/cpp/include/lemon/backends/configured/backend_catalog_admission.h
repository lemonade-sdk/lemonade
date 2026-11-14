#pragma once

#include "lemon/backends/configured/backend_definition_authorization.h"
#include "lemon/backends/configured/backend_definition_store.h"

namespace lemon::backends {

struct ExternalDefinitionInput {
    std::string_view bytes;
    std::string_view local_label;
};

struct CatalogAdmissionMetrics {
    std::size_t definitions = 0;
    std::size_t source_bytes = 0;
    std::size_t retained_logical_bytes = 0;
    std::size_t authorized_definitions = 0;
};

class CatalogSourceSnapshot;
class FrozenBuiltinCatalog;
struct CatalogAdmissionBuilder;

std::shared_ptr<const FrozenBuiltinCatalog> freeze_builtin_catalog();

class AdmittedCatalog {
public:
    const std::string& admission_id() const noexcept;
    const CatalogAdmissionMetrics& metrics() const noexcept;
    const CatalogSourceSnapshot& sources() const noexcept;
    std::shared_ptr<const BackendDefinition> v1_definition(const std::string& id) const;
    std::shared_ptr<const AuthorizedDefinition> v2_definition(const std::string& id) const;
    std::shared_ptr<const AuthorizedDefinition> builtin_v2_definition(const DefinitionKey& key) const;

private:
    AdmittedCatalog() = default;
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    friend struct CatalogAdmissionBuilder;
};

struct CatalogSnapshotResult {
    std::shared_ptr<const CatalogSourceSnapshot> snapshot;
    CompositionDiagnostics diagnostics;
};

struct CatalogAdmissionResult {
    std::shared_ptr<const AdmittedCatalog> catalog;
    CompositionDiagnostics diagnostics;
    bool resolved = false;
    bool authorized = false;
};

CatalogSnapshotResult make_catalog_source_snapshot(
    std::shared_ptr<const FrozenBuiltinCatalog> builtins,
    const std::vector<ExternalDefinitionInput>& sources,
    const DefinitionLimits& limits = {});
CatalogAdmissionResult admit_catalog(std::shared_ptr<const CatalogSourceSnapshot> snapshot);

struct CandidateAdmissionResult {
    CatalogAdmissionResult admission;
    unsigned input_schema_version = 0;
    std::string definition_id;
    std::string recipe;
};

CandidateAdmissionResult validate_candidate_definition(
    std::shared_ptr<const AdmittedCatalog> active, std::string_view bytes);

nlohmann::json installed_definition_admission_features();

}
