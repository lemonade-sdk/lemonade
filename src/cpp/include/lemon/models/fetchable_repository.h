#pragma once

#include "artifact_source.h"
#include "lemon/model_registry.h"
#include "artifact_layout.h"
#include "registry_id.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lemon {
namespace models {

using lemon::RegistryFile;

struct Revision {
    std::string id;
    bool pinned = false;    // an immutable commit SHA, not a mutable branch
};

struct RepoListing {
    std::string repo_id;
    Revision revision;
    std::string download_revision;  // spelling the host's file endpoint wants
    std::vector<RegistryFile> files;
};

// Argument to file_url().
struct RepoRef {
    std::string repo_id;
    Revision revision;
    std::string path;
};

// The repo's cache directory.
struct LocalRef {
    std::string repo_id;
};

struct Staleness {
    bool update_available = false;
    std::string local_snapshot_id;
    std::string remote_snapshot_id;
    std::vector<std::string> changed_files;
};

// The pullable role, layered on an artifact source.
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
    explicit FetchableRepository(ArtifactRef ref) : ArtifactSource(std::move(ref)) {}
};

// The files the ref needs, placed by the layout.
DownloadPlan registry_download_plan(const FetchableRepository& repository, const ArtifactRef& ref,
                                    const ArtifactLayout& layout, const Placement& placement,
                                    const RepoListing& listing);

// A factory takes the ref its source will carry.
using RepositoryFactory =
    std::function<std::unique_ptr<FetchableRepository>(const ArtifactRef& ref)>;

// Null when no registration for the id.
std::unique_ptr<FetchableRepository> create_repository(const RegistryId& id,
                                                       const ArtifactRef& ref);
std::vector<RegistryId> registered_repository_ids();

// Ids claimed twice; the first keeps serving.
std::vector<RegistryId> repository_registration_conflicts();

namespace detail {
void add_repository_factory(const RegistryId& id, RepositoryFactory factory);
}  // namespace detail

}  // namespace models
}  // namespace lemon

// Static initializer; must not throw, or an unknown host aborts startup.
#define LEMONADE_REGISTER_ARTIFACT_REPOSITORY_AT(line, ClassName, descriptor_expr)    \
    static const bool lemonade_repository_registered_##line##_##ClassName = [] {      \
        const ::lemon::models::RegistryDescriptor descriptor = descriptor_expr;   \
        ::lemon::models::detail::add_descriptor(descriptor);                      \
        ::lemon::models::detail::add_repository_factory(                          \
            descriptor.id, [](const ::lemon::models::ArtifactRef& ref) {          \
                return std::unique_ptr<::lemon::models::FetchableRepository>(     \
                    new ClassName(ref));                                          \
            });                                                                   \
        return true;                                                              \
    }()

// __LINE__ keeps two registrations of one class distinct.
#define LEMONADE_REGISTER_ARTIFACT_REPOSITORY_IMPL(line, ClassName, descriptor_expr) \
    LEMONADE_REGISTER_ARTIFACT_REPOSITORY_AT(line, ClassName, descriptor_expr)
#define LEMONADE_REGISTER_ARTIFACT_REPOSITORY(ClassName, descriptor_expr)              \
    LEMONADE_REGISTER_ARTIFACT_REPOSITORY_IMPL(__LINE__, ClassName, descriptor_expr)
