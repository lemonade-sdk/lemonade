#include <lemon/models/entry_identity.h>

#include <lemon/model_manager.h>

#include <algorithm>

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

// The two tables every API surface reads. `public_model_aliases` turns a name from
// a request into a cache key: the precedence-winner (Registered > Imported >
// Builtin) for a bare name, builtin.<X> for the bare key X, and
// ModelInfo::input_aliases entries; user.<X> and extra.<X> resolve through the
// cache lookup fallback and need no entry. `canonical_public_names` turns a cache
// key into the ID the API emits: the bare name for a winner, except a non-built-in
// collection, which emits its canonical-prefixed ID so the listed ID matches the
// one used to register, fetch and chat it; shadowed keys emit that too.
//
// Pure over an arbitrary model set, so build_cache() also runs it on the pre-filter
// set (#2748): a component's alias resolves the same way whether or not its model
// survives hardware filtering.
ModelAliasMaps compute_model_alias_maps(const std::map<std::string, ModelInfo>& models) {
    ModelAliasMaps result;

    struct Entry {
        std::string cache_key;
        EntryIdentity identity;
    };
    std::map<std::string, std::vector<Entry>> by_bare;

    for (const auto& [cache_key, info] : models) {
        EntryIdentity identity = entry_identity(cache_key, info);
        by_bare[identity.bare].push_back({cache_key, std::move(identity)});
    }

    for (auto& [bare, entries] : by_bare) {
        std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return entry_precedence_rank(a.identity) <
                   entry_precedence_rank(b.identity);
        });

        const Entry& winner = entries.front();
        result.public_model_aliases[bare] = winner.cache_key;

        const auto winner_it = models.find(winner.cache_key);
        const bool winner_is_prefixed_collection =
            winner.identity.source != ModelSource::Builtin &&
            winner_it != models.end() &&
            is_model_collection_recipe(winner_it->second.recipe);
        result.canonical_public_names[winner.cache_key] =
            winner_is_prefixed_collection ? canonical_id(winner.identity.source, bare) : bare;

        for (size_t i = 1; i < entries.size(); ++i) {
            const Entry& shadowed = entries[i];
            std::string canonical = canonical_id(shadowed.identity.source, bare);
            result.canonical_public_names[shadowed.cache_key] = canonical;
            if (canonical != shadowed.cache_key) {
                result.public_model_aliases[canonical] = shadowed.cache_key;
            }
        }
    }

    for (const auto& [cache_key, info] : models) {
        if (parse_canonical_id(cache_key)) continue;
        if (entry_identity(cache_key, info).qualifies_itself) continue;
        std::string canonical = canonical_id(ModelSource::Builtin, cache_key);
        result.public_model_aliases.try_emplace(canonical, cache_key);
    }

    // Only an unqualified built-in can be pushed aside by an input alias; a
    // provider-qualified id already carries where it came from.
    auto demotable_builtin = [&models](const std::string& cache_key) {
        auto it = models.find(cache_key);
        if (it == models.end()) return false;
        const EntryIdentity identity = entry_identity(cache_key, it->second);
        return identity.source == ModelSource::Builtin && !identity.qualifies_itself;
    };

    for (const auto& [cache_key, info] : models) {
        for (const auto& alias : info.input_aliases) {
            if (parse_canonical_id(alias)) {
                if (models.find(alias) == models.end()) {
                    result.public_model_aliases[alias] = cache_key;
                }
            } else {
                auto existing = result.public_model_aliases.find(alias);
                if (existing == result.public_model_aliases.end()) {
                    result.public_model_aliases[alias] = cache_key;
                    continue;
                }

                if (demotable_builtin(existing->second)) {
                    std::string builtin_canonical = canonical_id(ModelSource::Builtin, alias);
                    result.canonical_public_names[existing->second] = builtin_canonical;
                    result.public_model_aliases[builtin_canonical] = existing->second;
                    result.public_model_aliases[alias] = cache_key;
                }
            }
        }
    }

    return result;
}

}  // namespace models
}  // namespace lemon
