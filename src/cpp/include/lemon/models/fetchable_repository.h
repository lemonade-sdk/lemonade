#pragma once

#include "artifact_source.h"
#include "registry_id.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lemon {
namespace models {

struct Revision {
    std::string id;
    bool pinned = false;    // an immutable commit SHA, not a mutable branch
};

struct RegistryFile {
    std::string path;
    std::int64_t size = 0;
    std::string hash_algorithm;
    std::string hash;
    bool directory = false;
};

struct RepoListing {
    std::string repo_id;
    Revision revision;
    std::vector<RegistryFile> files;
};

// A file at a revision on the host: the argument to file_url().
struct RepoRef {
    std::string repo_id;
    Revision revision;
    std::string path;
};

// A repository's local namespace: the cache directory holding that repo's bytes.
struct LocalRef {
    std::string repo_id;
};

struct Staleness {
    bool update_available = false;
    std::string local_snapshot_id;
    std::string remote_snapshot_id;
    std::vector<std::string> changed_files;
};

// The pullable role, layered on an artifact source. What a host owns is the
// protocol and its own vocabulary; transfer, layout and blob naming stay shared,
// because duplicating them per host re-creates a god object.
class FetchableRepository : public ArtifactSource {
  public:
    virtual RegistryId id() const = 0;
    virtual Revision default_revision() const = 0;

    virtual RepoListing list(const std::string& repo_id,
                             const std::string& revision = "") const = 0;
    virtual std::string file_url(const RepoRef& ref) const = 0;
    virtual std::map<std::string, std::string> auth_headers() const { return {}; }

    virtual bool can_go_stale() const = 0;
    virtual Revision active_revision(const LocalRef& local) const = 0;
    virtual void set_active_revision(const LocalRef& local, const Revision& revision) = 0;
    virtual Staleness compare(const LocalRef& local, const RepoListing& listing) const = 0;
    virtual void commit(const LocalRef& local, const Revision& revision) = 0;

    virtual std::string cache_dir_name(const std::string& repo_id) const = 0;

    virtual bool supports_search() const { return false; }

  protected:
    // A source is built for one checkpoint, so the ref arrives at construction
    // and the role methods take no arguments.
    explicit FetchableRepository(ArtifactRef ref) : ArtifactSource(std::move(ref)) {}
};

using RepositoryFactory = std::function<std::unique_ptr<FetchableRepository>()>;

// Builds the repository a host names, or null when none registered.
std::unique_ptr<FetchableRepository> create_repository(const RegistryId& id);
std::vector<RegistryId> registered_repository_ids();

// Hosts claiming a factory more than once: a bug in two registrations, reported
// at startup rather than resolved by whichever static initializer ran last.
std::vector<RegistryId> repository_registration_conflicts();

namespace detail {
void add_repository_factory(const RegistryId& id, RepositoryFactory factory);
}  // namespace detail

}  // namespace models
}  // namespace lemon

// Static initializer, no dlopen, no plugin boundary. A host the build does not
// recognize skips its models with a warning rather than aborting startup, so
// nothing here is allowed to throw.
#define LEMONADE_REGISTER_ARTIFACT_REPOSITORY(ClassName, descriptor_expr)         \
    static const bool lemonade_repository_registered_##ClassName = [] {           \
        const ::lemon::models::RegistryDescriptor descriptor = descriptor_expr;   \
        ::lemon::models::detail::add_descriptor(descriptor);                      \
        ::lemon::models::detail::add_repository_factory(                          \
            descriptor.id, [] {                                                   \
                return std::unique_ptr<::lemon::models::FetchableRepository>(     \
                    new ClassName());                                             \
            });                                                                   \
        return true;                                                              \
    }()
