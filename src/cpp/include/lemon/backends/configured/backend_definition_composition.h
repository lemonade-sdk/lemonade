#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon::backends {

enum class DefinitionNamespace { Builtin, External };

struct DefinitionKey {
    DefinitionNamespace space;
    std::string id;
    std::uint32_t contract = 0;
};

struct DefinitionSourceView {
    DefinitionKey key;
    std::string_view bytes;
    std::string_view local_label;
};

struct SourceLocation {
    DefinitionKey document;
    std::string pointer;
};

struct RecordKey {
    enum class Field { Id, Name };
    Field field;
    std::string value;
};

struct DefinitionTarget {
    std::string path;
    std::optional<RecordKey> match;
    std::string at;
};

enum class DefinitionEditKind { Merge, Replace, Clear, Remove, Add };

struct AuthorshipEvent {
    DefinitionEditKind kind;
    SourceLocation source;
    DefinitionTarget target;
    bool whole_subtree = false;
};

struct CompositionDiagnostic {
    std::string code;
    std::string definition;
    std::string pointer;
    std::string message;
    std::optional<std::string> effective_pointer;
    std::optional<SourceLocation> origin;
    std::vector<SourceLocation> related;
};

struct CompositionDiagnostics {
    std::vector<CompositionDiagnostic> entries;
    bool truncated = false;
};

struct DefinitionLimits {
    std::size_t source_bytes = 1024 * 1024;
    std::size_t chain_documents = 8;
    std::size_t closure_source_bytes = 8 * 1024 * 1024;
    std::size_t source_depth = 64;
    std::size_t closure_source_nodes = 262144;
    std::size_t edits_per_source = 1024;
    std::size_t pointer_bytes = 4096;
    std::size_t effective_depth = 64;
    std::size_t effective_bytes = 1024 * 1024;
    std::size_t effective_nodes = 65536;
    std::size_t work_bytes = 16 * 1024 * 1024;
    std::size_t work_nodes = 1048576;
    std::size_t diagnostics = 100;
    std::size_t snapshot_documents = 1024;
    std::size_t snapshot_source_bytes = 64 * 1024 * 1024;
    std::size_t retained_logical_bytes = 128 * 1024 * 1024;
};

struct ResolutionMetrics {
    std::size_t closure_source_bytes = 0;
    std::size_t closure_source_nodes = 0;
    std::size_t effective_bytes = 0;
    std::size_t effective_nodes = 0;
    std::size_t work_bytes = 0;
    std::size_t work_nodes = 0;
    std::size_t retained_logical_bytes = 0;
};

struct DependencyIdentity {
    DefinitionKey document;
    std::string source_digest;
    std::string definition_hash;
};

class ResolutionProvenance {
public:
    std::optional<SourceLocation>
    origin_at(std::string_view effective_pointer) const;

    std::optional<SourceLocation>
    removal_at(const DefinitionTarget& target) const;

    const std::vector<AuthorshipEvent>& authorship() const noexcept;

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    friend class DefinitionResolver;
    friend class DefinitionAuthorizer;
};

struct ResolvedDefinition {
    DefinitionKey key;
    nlohmann::json declaration;
    std::string source_digest;
    std::string definition_hash;
    std::string dependency_digest;
    std::vector<DependencyIdentity> dependencies;
    std::shared_ptr<const ResolutionProvenance> provenance;
    ResolutionMetrics metrics;
};

struct DefinitionResolutionResult {
    std::shared_ptr<const ResolvedDefinition> definition;
    CompositionDiagnostics diagnostics;
};

}
