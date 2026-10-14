#include <lemon/models/fetchable_repository.h>

#include <lemon/backends/hf_cache_util.h>
#include <lemon/model_registry.h>
#include <lemon/utils/path_utils.h>

#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

RegistryDescriptor huggingface_value() {
    return RegistryDescriptor{RegistryId("huggingface"),
                              {"huggingface", "hf", ""},
                              "Hugging Face",
                              "hf",
                              "models--",
                              "main",
                              "https://huggingface.co"};
}

RegistryDescriptor modelscope_value() {
    return RegistryDescriptor{RegistryId("modelscope"),
                              {"modelscope", "ms"},
                              "ModelScope",
                              "ms",
                              "modelscope--models--",
                              "master",
                              ""};
}

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

// Hosts differ in protocol, and ModelRegistry already holds that difference, so
// one class serves every host it knows. A host needing divergent behavior
// subclasses this rather than adding a test on its id.
class RegistryBackedRepository : public FetchableRepository {
  public:
    explicit RegistryBackedRepository(ArtifactRef ref) : FetchableRepository(std::move(ref)) {}

    ArtifactKind kind() const override { return ArtifactKind::Registry; }

    std::string resolve() const override { return utils::path_to_utf8(active_snapshot()); }

    Presence present() const override {
        const fs::path snapshot = active_snapshot();
        if (snapshot.empty()) {
            return Presence{false, "", "no active snapshot for " + ref_.repo_id};
        }
        return Presence{true, utils::path_to_utf8(snapshot), ""};
    }

    Acquisition acquisition() const override {
        // A plan names files, and which files a checkpoint needs is the layout's
        // answer; until layout_for() exists this cannot say anything true.
        throw std::logic_error("RegistryBackedRepository::acquisition needs ArtifactLayout");
    }

    Deletion deletion() const override { return Deletion{Deletion::Kind::CacheDir, ""}; }

    RegistryId id() const override { return host(); }

    Revision default_revision() const override { return Revision{registry().default_revision(), false}; }

    RepoListing list(const std::string& repo_id, const std::string& revision) const override {
        const RegistryRepository repository = registry().fetch_repository(repo_id, revision);
        RepoListing listing;
        listing.repo_id = repo_id;
        listing.revision = Revision{repository.snapshot_id, is_commit_sha(repository.snapshot_id)};
        listing.files = repository.files;
        return listing;
    }

    std::string file_url(const RepoRef& ref) const override {
        return registry().resolve_file_url(ref.repo_id, ref.revision.id, ref.path);
    }

    std::map<std::string, std::string> auth_headers() const override {
        return registry().auth_headers();
    }

    bool can_go_stale() const override { return true; }

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

    fs::path cache_dir(const std::string& repo_id) const {
        return utils::path_from_utf8(utils::get_hf_cache_dir()) / cache_dir_name(repo_id);
    }

    fs::path active_snapshot() const {
        return backends::hf_cache::active_snapshot_path(cache_dir(ref_.repo_id));
    }
};

LEMONADE_REGISTER_ARTIFACT_REPOSITORY(RegistryBackedRepository, huggingface_value());
LEMONADE_REGISTER_ARTIFACT_REPOSITORY(RegistryBackedRepository, modelscope_value());

}  // namespace models
}  // namespace lemon
