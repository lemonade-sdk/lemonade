#include <lemon/models/entry_identity.h>

#include <lemon/model_manager.h>

namespace lemon {
namespace models {

EntryIdentity entry_identity(const std::string& cache_key, const ModelInfo& info) {
    EntryIdentity identity;
    if (auto canonical = parse_canonical_id(cache_key)) {
        identity.source = canonical->source;
        identity.bare = canonical->bare_name;
        return identity;
    }
    identity.bare = cache_key;
    identity.qualifies_itself = !info.cloud_provider.empty();
    return identity;
}

int entry_precedence_rank(const EntryIdentity& identity) {
    if (identity.qualifies_itself) return 1;
    switch (identity.source) {
        case ModelSource::Registered: return 0;
        case ModelSource::Imported: return 2;
        case ModelSource::Builtin: return 3;
    }
    return 99;
}

}  // namespace models
}  // namespace lemon
