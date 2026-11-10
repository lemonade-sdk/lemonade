// The spec's acceptance fixture: a pullable host that no component knows about. The
// repository, its descriptor and its registration all live in this file. If making it
// work ever edits ModelStore, ModelSyncService, ModelIngest, ModelManager or ModelInfo,
// the axis this exists to prove has gone.
#include <lemon/models/artifact_layout.h>
#include <lemon/models/fetchable_repository.h>
#include <lemon/utils/path_utils.h>

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

const RegistryId& host_id() {
    static const RegistryId id("fixture-hub");
    return id;
}

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

// The directory this host serves, and the endpoint it serves it over. An endpoint is
// optional on purpose: with none the bytes are already reachable as files, which is the
// case a transfer that never opens a socket has to serve.
std::string served_root() { return environment("LEMONADE_FIXTURE_HUB_ROOT"); }
std::string served_endpoint() { return environment("LEMONADE_FIXTURE_HUB_ENDPOINT"); }

std::string forward_slashes(const std::string& path) {
    std::string out = path;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return out;
}

// This host's own staleness basis: a fingerprint of the paths and sizes it serves, so
// adding a file mints a new revision the way a new commit would.
std::string fingerprint(const std::vector<std::string>& lines) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto& line : lines) {
        for (const char c : line) {
            hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        }
    }
    std::ostringstream out;
    out << "fx" << std::hex << hash;
    return out.str();
}

std::uint64_t size_of(const fs::path& path) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

}  // namespace

// Kept out of the anonymous namespace so the fixture's own test can name it; nothing in
// src/cpp does.
class FixtureHubRepository : public FetchableRepository {
  public:
    FixtureHubRepository(ArtifactRef ref, SourcePlacement placement)
        : FetchableRepository(std::move(ref), placement) {}

    ArtifactKind kind() const override { return ArtifactKind::Registry; }

    RegistryId id() const override { return host_id(); }

    Revision default_revision() const override { return Revision{"main", false}; }

    std::string cache_dir_name(const std::string& repo_id) const override {
        std::string flat = forward_slashes(repo_id);
        for (char& c : flat) {
            if (c == '/') c = '-';
        }
        return "fixture-hub--" + flat;
    }

    RepoListing list(const std::string& repo_id, const std::string&) const override {
        RepoListing listing;
        listing.repo_id = repo_id;
        const fs::path root = source_dir(repo_id);
        std::vector<std::string> fingerprinted;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            throw std::runtime_error("Fixture hub serves no repository " + repo_id);
        }
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            RegistryFile file;
            file.path = forward_slashes(fs::relative(entry.path(), root).string());
            file.directory = entry.is_directory();
            file.size = file.directory ? 0 : size_of(entry.path());
            fingerprinted.push_back(file.path + "|" + std::to_string(file.size));
            listing.files.push_back(file);
        }
        const std::string revision = fingerprint(fingerprinted);
        listing.revision = Revision{revision, true};
        listing.download_revision = revision;
        return listing;
    }

    std::string file_url(const RepoRef& ref) const override {
        const std::string endpoint = served_endpoint();
        if (!endpoint.empty()) {
            return endpoint + "/" + forward_slashes(ref.repo_id) + "/" + ref.path;
        }
        return "file://" + forward_slashes((source_dir(ref.repo_id) / ref.path).string());
    }

    // The credential travels with the call and never with the URL, which is what keeps a
    // plan loggable.
    std::map<std::string, std::string> auth_headers() const override {
        const std::string token = environment("LEMONADE_FIXTURE_HUB_TOKEN");
        if (token.empty()) {
            return {};
        }
        return {{"Authorization", "Bearer " + token}};
    }

    bool can_go_stale() const override { return true; }

    Revision active_revision(const LocalRef& local) const override {
        std::ifstream in(cache_dir(local.repo_id) / "refs" / "main");
        std::string recorded;
        std::getline(in, recorded);
        return Revision{recorded, recorded.rfind("fx", 0) == 0};
    }

    void set_active_revision(const LocalRef& local, const Revision& revision) override {
        const fs::path refs = cache_dir(local.repo_id) / "refs";
        fs::create_directories(refs);
        std::ofstream out(refs / "main", std::ios::binary | std::ios::trunc);
        out << revision.id;
    }

    Staleness compare(const LocalRef& local, const RepoListing& listing) const override {
        Staleness staleness;
        staleness.local_snapshot_id = active_revision(local).id;
        staleness.remote_snapshot_id = listing.revision.id;
        staleness.update_available = !staleness.local_snapshot_id.empty() &&
                                     !staleness.remote_snapshot_id.empty() &&
                                     staleness.local_snapshot_id != staleness.remote_snapshot_id;
        if (!staleness.update_available) {
            return staleness;
        }
        const fs::path snapshot =
            cache_dir(local.repo_id) / "snapshots" / staleness.local_snapshot_id;
        for (const auto& file : listing.files) {
            if (file.directory) continue;
            const fs::path local_copy = snapshot / file.path;
            std::error_code ec;
            if (!fs::exists(local_copy, ec) || size_of(local_copy) != file.size) {
                staleness.changed_files.push_back(file.path);
            }
        }
        return staleness;
    }

    void commit(const LocalRef& local, const Revision& revision) override {
        set_active_revision(local, revision);
    }

    std::string resolve() const override {
        Placement where = placement();
        where.revision = active_revision(LocalRef{ref_.repo_id}).id;
        const std::string target = layout().target(where, ref_.variant);
        if (!target.empty()) {
            return target;
        }
        return utils::path_to_utf8(cache_dir(ref_.repo_id) / "snapshots" / where.revision);
    }

    Presence present() const override {
        const std::string target = resolve();
        std::error_code ec;
        if (target.empty() || !fs::exists(utils::path_from_utf8(target), ec)) {
            return Presence{false, "", "no bytes for " + ref_.repo_id + " at its active revision"};
        }
        return Presence{true, target, ""};
    }

    Acquisition acquisition() const override {
        Acquisition acquisition;
        acquisition.kind = Acquisition::Kind::Plan;
        RepoListing listing = list(ref_.repo_id, ref_.revision);
        Placement where = placement();
        where.revision = listing.revision.id;
        acquisition.plan = registry_download_plan(*this, ref_, layout(), where, listing);
        return acquisition;
    }

    Deletion deletion() const override { return Deletion{Deletion::Kind::CacheDir, ""}; }

  private:
    fs::path source_dir(const std::string& repo_id) const {
        const fs::path root = utils::path_from_utf8(served_root());
        const fs::path candidate = utils::path_from_utf8(repo_id);
        return candidate.is_absolute() ? candidate : root / candidate;
    }

    fs::path cache_dir(const std::string& repo_id) const {
        return utils::path_from_utf8(utils::get_hf_cache_dir()) / cache_dir_name(repo_id);
    }

    Placement placement() const {
        Placement placement;
        placement.cache_dir = utils::path_to_utf8(cache_dir(ref_.repo_id));
        placement.sibling = sibling();
        return placement;
    }
};

RegistryDescriptor fixture_hub_descriptor() {
    RegistryDescriptor descriptor;
    descriptor.id = host_id();
    descriptor.aliases = {"fixture-hub", "fh"};
    descriptor.display_name = "Fixture Hub";
    descriptor.scheme_prefix = "fh";
    descriptor.cache_dir_prefix = "fixture-hub--";
    descriptor.default_revision = "main";
    descriptor.endpoint_default = "";
    descriptor.download_protocol = "fixture-hub";
    descriptor.doc = DocEntry{
        "fixture-hub",
        "Fixture Hub",
        "A test host that serves one directory, over `file://` or over HTTP.",
        {"It exists to prove a host can be added without touching a component."},
        "{endpoint}/{repo}/{path}"};
    descriptor.env = {
        EnvVar{"LEMONADE_FIXTURE_HUB_ROOT", "the directory served"},
        EnvVar{"LEMONADE_FIXTURE_HUB_ENDPOINT", "an HTTP endpoint, absent for a local copy"},
        EnvVar{"LEMONADE_FIXTURE_HUB_TOKEN", "read access, sent as a bearer token"}};
    descriptor.revision_semantics = "a fingerprint of the served paths and sizes";
    descriptor.staleness_basis = "that fingerprint against the one recorded in `refs/main`";
    return descriptor;
}

LEMONADE_REGISTER_ARTIFACT_REPOSITORY(FixtureHubRepository, fixture_hub_descriptor());

}  // namespace models
}  // namespace lemon
