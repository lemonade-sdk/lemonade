#include <lemon/models/fetchable_repository.h>

#include <lemon/backends/hf_cache_util.h>
#include "../model_registry.h"
#include <lemon/models/registry_descriptors.h>
#include <lemon/registry_files.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>

#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

// A full commit SHA cannot move; a branch name can.
bool is_commit_sha(const std::string& revision) {
    if (revision.size() != 40) {
        return false;
    }
    for (const char c : revision) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

DownloadPlan registry_download_plan(const FetchableRepository& repository, const ArtifactRef& ref,
                                    const ArtifactLayout& layout, const Placement& placement,
                                    const RepoListing& listing) {
    std::vector<std::string> available;
    available.reserve(listing.files.size());
    std::map<std::string, const RegistryFile*> by_name;
    for (const auto& file : listing.files) {
        if (file.directory) {
            continue;
        }
        available.push_back(file.path);
        by_name.emplace(file.path, &file);
    }

// A side checkpoint names its one file; a main one takes the recipe's selection.
    const bool side_checkpoint = !ref.checkpoint_type.empty() && ref.checkpoint_type != "main";
    std::vector<std::string> wanted;
    if (side_checkpoint) {
        if (by_name.find(ref.variant) == by_name.end()) {
            throw std::runtime_error("Checkpoint file not found in repository " + ref.repo_id +
                                     ": " + ref.variant);
        }
        wanted.push_back(ref.variant);
    } else {
        wanted = registry_files::select_main_repo_files(ref.repo_id, ref.recipe, ref.variant,
                                                        available);
    }

    DownloadPlan plan;
    plan.download_path = layout.directory(placement);
    const Revision revision{listing.download_revision.empty() ? listing.revision.id
                                                              : listing.download_revision,
                            listing.revision.pinned};
    for (const auto& name : wanted) {
        DownloadFile file;
        file.name = name;
        file.url = repository.file_url(RepoRef{ref.repo_id, revision, name});
        if (const auto found = by_name.find(name); found != by_name.end()) {
            file.size = found->second->size;
            file.expected_hash_algorithm = found->second->hash_algorithm;
            file.expected_hash = found->second->hash;
        }
        plan.files.push_back(file);
    }
    return plan;
}

// ModelRegistry already holds the protocol difference; a diverging host subclasses.
class RegistryBackedRepository : public FetchableRepository {
  public:
    RegistryBackedRepository(ArtifactRef ref, SourcePlacement placement)
        : FetchableRepository(std::move(ref), placement) {}

    ArtifactKind kind() const override { return ArtifactKind::Registry; }

    std::string resolve() const override { return local_target(); }

    Presence present() const override {
        const std::string target = local_target();
        if (target.empty()) {
            return Presence{false, "", "no active snapshot for " + ref_.repo_id};
        }
        if (!utils::safe_exists(utils::path_from_utf8(target))) {
            return Presence{false, "", "no bytes at " + target};
        }
        return Presence{true, target, ""};
    }

    Acquisition acquisition() const override {
        const RepoListing listing = list(ref_.repo_id, ref_.revision);
        Placement where = placement();
        where.revision =
            listing.revision.id.empty() ? listing.download_revision : listing.revision.id;

        Acquisition acquisition;
        acquisition.kind = Acquisition::Kind::Plan;
        acquisition.plan = registry_download_plan(*this, ref_, layout(), where, listing);
        return acquisition;
    }

    Deletion deletion() const override { return Deletion{Deletion::Kind::CacheDir, ""}; }

    RegistryId id() const override { return host(); }

    const ArtifactRef& artifact_ref() const { return ref_; }

    Revision default_revision() const override { return Revision{registry().default_revision(), false}; }

    RepoListing list(const std::string& repo_id, const std::string& revision) const override {
        const RegistryRepository repository = registry().fetch_repository(repo_id, revision);
        RepoListing listing;
        listing.repo_id = repo_id;
        listing.revision = Revision{repository.snapshot_id, is_commit_sha(repository.snapshot_id)};
        listing.download_revision = repository.revision;
        listing.files = repository.files;
        listing.raw_metadata = repository.raw_metadata;
        return listing;
    }

    std::string file_url(const RepoRef& ref) const override {
        return registry().resolve_file_url(ref.repo_id, ref.revision.id, ref.path);
    }

    std::map<std::string, std::string> auth_headers() const override {
        return registry().auth_headers();
    }

    bool can_go_stale() const override { return true; }

    bool supports_search() const override {
        const auto* descriptor = models::descriptor_for(host());
        return descriptor != nullptr && descriptor->supports_search;
    }

    RegistrySearchResponse search(const std::string& query, std::size_t limit,
                                  bool gguf_only) const override {
        return search_registry_models(host(), query, limit, gguf_only);
    }

    Revision active_revision(const LocalRef& local) const override {
        const std::string recorded =
            backends::hf_cache::read_ref_main(cache_dir(local.repo_id));
        return Revision{recorded, is_commit_sha(recorded)};
    }

    void set_active_revision(const LocalRef& local, const Revision& revision) override {
        backends::hf_cache::write_ref_main(cache_dir(local.repo_id), revision.id);
    }

    Staleness compare(const LocalRef& local, const RepoListing& listing) const override {
        Staleness staleness;
        staleness.local_snapshot_id = active_revision(local).id;
        staleness.remote_snapshot_id = listing.revision.id;
        staleness.update_available = !staleness.local_snapshot_id.empty() &&
                                     !staleness.remote_snapshot_id.empty() &&
                                     staleness.local_snapshot_id != staleness.remote_snapshot_id;
        return staleness;
    }

    void commit(const LocalRef& local, const Revision& revision) override {
        set_active_revision(local, revision);
    }

    std::string cache_dir_name(const std::string& repo_id) const override {
        return registry_repo_cache_dir_name(repo_id, host());
    }

  private:
    const RegistryId& host() const {
        if (ref_.registry == std::nullopt) {
            throw std::logic_error("A registry source needs a host in its ArtifactRef");
        }
        return *ref_.registry;
    }

    const ModelRegistry& registry() const { return model_registry(host()); }

    Placement placement() const {
        Placement placement;
        placement.cache_dir = utils::path_to_utf8(cache_dir(ref_.repo_id));
        placement.sibling = sibling();
        return placement;
    }

// Falls back to the snapshot directory when no file is named.
    std::string local_target() const {
        Placement where = placement();
        where.revision = active_revision(LocalRef{ref_.repo_id}).id;
        const std::string target = layout().target(where, ref_.variant);
        return target.empty() ? utils::path_to_utf8(active_snapshot()) : target;
    }

    fs::path cache_dir(const std::string& repo_id) const {
        return utils::path_from_utf8(utils::get_hf_cache_dir()) / cache_dir_name(repo_id);
    }

    fs::path active_snapshot() const {
        return backends::hf_cache::active_snapshot_path(cache_dir(ref_.repo_id));
    }
};

LEMONADE_REGISTER_ARTIFACT_REPOSITORY(RegistryBackedRepository,
                                              builtin_registries::huggingface());
LEMONADE_REGISTER_ARTIFACT_REPOSITORY(RegistryBackedRepository,
                                              builtin_registries::modelscope());

}  // namespace models
}  // namespace lemon
