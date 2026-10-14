#include <lemon/models/fetchable_repository.h>

#include <map>
#include <utility>

namespace lemon {
namespace models {

namespace {

// Written during static initialization, read afterwards.
std::map<RegistryId, RepositoryFactory>& factories() {
    static std::map<RegistryId, RepositoryFactory> registered;
    return registered;
}

std::vector<RegistryId>& conflicts() {
    static std::vector<RegistryId> claimed_twice;
    return claimed_twice;
}

}  // namespace

std::unique_ptr<FetchableRepository> create_repository(const RegistryId& id,
                                                      const ArtifactRef& ref) {
    const auto it = factories().find(id);
    if (it == factories().end()) {
        return nullptr;
    }
    ArtifactRef stamped = ref;
    stamped.registry = id;
    return it->second(stamped);
}

std::vector<RegistryId> registered_repository_ids() {
    std::vector<RegistryId> ids;
    ids.reserve(factories().size());
    for (const auto& entry : factories()) {
        ids.push_back(entry.first);
    }
    return ids;
}

std::vector<RegistryId> repository_registration_conflicts() { return conflicts(); }

namespace detail {

void add_repository_factory(const RegistryId& id, RepositoryFactory factory) {
    // The first registration keeps serving; the loser is reported.
    if (!factories().emplace(id, std::move(factory)).second) {
        conflicts().push_back(id);
    }
}

}  // namespace detail

}  // namespace models
}  // namespace lemon
