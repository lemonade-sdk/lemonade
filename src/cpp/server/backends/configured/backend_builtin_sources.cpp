#include "backend_builtin_source_resources.h"
#include "lemon/backends/configured/backend_builtin_sources.h"
#include "lemon/backends/configured/backend_definition_resolver.h"

#include <stdexcept>

namespace lemon::backends {
namespace {
class ResolvedSources {
public:
    ResolvedSources() {
        const auto snapshot = make_v2_source_snapshot(builtin_source_views());
        if (!snapshot.snapshot) throw std::logic_error("Invalid embedded builtin source snapshot");
        DefinitionResolver resolver(snapshot.snapshot);
        for (const auto& source : builtin_source_views()) {
            const auto result = resolver.resolve(source.key);
            if (!result.definition) {
                const auto detail = result.diagnostics.entries.empty() ? "" :
                    ": " + result.diagnostics.entries.front().code + " " + result.diagnostics.entries.front().pointer;
                throw std::logic_error("Invalid embedded builtin resolved source: " + source.key.id + detail);
            }
            definitions_.push_back(result.definition);
        }
    }
    const ResolvedDefinition& get(const DefinitionKey& key) const {
        for (const auto& definition : definitions_) {
            const auto& found = definition->key;
            if (found.space == key.space && found.id == key.id && found.contract == key.contract) return *definition;
        }
        throw std::logic_error("Unknown embedded builtin resolved source: " + key.id);
    }
private:
    std::vector<std::shared_ptr<const ResolvedDefinition>> definitions_;
};
}

const ResolvedDefinition& builtin_resolved_source(const DefinitionKey& key) {
    static const ResolvedSources sources;
    return sources.get(key);
}

}
