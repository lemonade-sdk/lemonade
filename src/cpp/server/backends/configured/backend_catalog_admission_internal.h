#pragma once

#include "backend_definition_exports_internal.h"
#include "lemon/backends/configured/backend_catalog_admission.h"

#include <map>
#include <set>

namespace lemon::backends {

class CatalogAdmissionTicket final {
private:
    CatalogAdmissionTicket() = default;
    friend struct CatalogAdmissionBuilder;
};

struct CatalogInputHeader {
    unsigned version = 0;
    std::string id;
    std::string recipe;
    nlohmann::json parent;
    std::size_t nodes = 0;
};
nlohmann::json public_admission_diagnostic(const CompositionDiagnostic& diagnostic);
CatalogInputHeader inspect_catalog_input(std::string_view bytes, const DefinitionLimits& limits);

class FrozenBuiltinCatalog {
private:
    FrozenBuiltinCatalog() = default;
    std::set<std::string> ids_, recipes_, sections_, options_;
    std::map<std::string, std::set<std::string>> persisted_;
    std::shared_ptr<const FrozenDefinitionSnapshot> authority_;
    nlohmann::json defaults_, pins_;
    std::size_t definitions_ = 0, source_bytes_ = 0, logical_bytes_ = 0;
    std::string identity_;
    friend struct CatalogAdmissionBuilder;
    friend class BackendCatalog;
    friend struct CatalogExecutionBuilder;
};

class CatalogSourceSnapshot {
private:
    struct Source {
        std::string bytes;
        CatalogInputHeader header;
        std::shared_ptr<const BackendDefinition> v1;
    };
    std::shared_ptr<const FrozenBuiltinCatalog> builtins_;
    DefinitionLimits limits_;
    std::vector<Source> sources_;
    std::map<std::string, std::size_t> ids_;
    std::map<std::string, std::vector<std::string>> descendants_;
    CatalogAdmissionMetrics metrics_;
    std::shared_ptr<const FrozenDefinitionSnapshot> authority_;
    friend struct CatalogAdmissionBuilder;
    friend class BackendCatalog;
    friend struct CatalogExecutionBuilder;
};

struct CatalogAdmissionBuilder {
    static std::shared_ptr<const FrozenBuiltinCatalog> freeze();
    static CatalogSnapshotResult snapshot(std::shared_ptr<const FrozenBuiltinCatalog> builtins,
        const std::vector<ExternalDefinitionInput>& sources, const DefinitionLimits& limits);
    static CatalogAdmissionResult admit(std::shared_ptr<const CatalogSourceSnapshot> snapshot);
    static CandidateAdmissionResult candidate(std::shared_ptr<const AdmittedCatalog> active, std::string_view bytes);
    static std::string authority_identity(const AuthorizedDefinition& definition);
    static bool contains_token(const AdmittedCatalog& catalog, const AuthorizedDefinition& token) noexcept;
    static std::shared_ptr<const AuthorizedDefinition> attach_ticket(
        std::shared_ptr<const AuthorizedDefinition> token, std::shared_ptr<const CatalogAdmissionTicket> ticket);
    static std::vector<std::shared_ptr<const ResolvedDefinition>> builtin_definitions(const FrozenDefinitionSnapshot& snapshot);
#ifdef LEMON_BACKEND_EXPORT_TESTING
    static void test_detach_ticket(const AuthorizedDefinition& token);
    static std::shared_ptr<const AuthorizedDefinition> test_clone_token(const AuthorizedDefinition& token);
#endif
    static std::shared_ptr<const FrozenBuiltinCatalog> test_builtins(
        std::shared_ptr<const FrozenDefinitionSnapshot> authority, const std::vector<DefinitionSourceView>& sources);
};

}
