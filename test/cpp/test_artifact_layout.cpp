#include "lemon/model_manager.h"
#include "lemon/models/artifact_layout.h"
#include "lemon/models/artifact_resolver.h"
#include "lemon/models/fetchable_repository.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using lemon::ModelInfo;
using lemon::models::ArtifactKind;
using lemon::models::ArtifactRef;
using lemon::models::ArtifactSourceResolver;
using lemon::models::Acquisition;
using lemon::models::CheckpointRef;
using lemon::models::Deletion;
using lemon::models::DownloadPlan;
using lemon::models::FetchableRepository;
using lemon::models::hf_hub_layout;
using lemon::models::LocalRef;
using lemon::models::Placement;
using lemon::models::Presence;
using lemon::models::registry_download_plan;
using lemon::models::RegistryId;
using lemon::models::RepoListing;
using lemon::models::RepoRef;
using lemon::models::Revision;
using lemon::models::sibling_layout;
using lemon::models::Staleness;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

namespace fs = std::filesystem;

class ListingRepository : public FetchableRepository {
  public:
    explicit ListingRepository(ArtifactRef ref) : FetchableRepository(std::move(ref)) {}

    ArtifactKind kind() const override { return ArtifactKind::Registry; }
    std::string resolve() const override { return ""; }
    Presence present() const override { return Presence{}; }
    Acquisition acquisition() const override { return Acquisition{}; }
    Deletion deletion() const override { return Deletion{}; }

    RegistryId id() const override { return RegistryId("huggingface"); }
    Revision default_revision() const override { return Revision{"main", false}; }

    RepoListing list(const std::string&, const std::string&) const override {
        RepoListing listing;
        listing.repo_id = "org/repo";
        listing.revision = Revision{"snapshot-1", true};
        listing.download_revision = "rev-sha";
        listing.files = {
            {"model-Q4_K_M.gguf", 100, "sha256", "beef", false},
            {"model-Q8_0.gguf", 200, "", "", false},
            {"config.json", 5, "", "", false},
            {"assets", 0, "", "", true},
        };
        return listing;
    }

    std::string file_url(const RepoRef& ref) const override {
        return "https://host.example/" + ref.repo_id + "/resolve/" + ref.revision.id + "/" +
               ref.path;
    }

    bool can_go_stale() const override { return true; }
    Revision active_revision(const LocalRef&) const override { return Revision{}; }
    void set_active_revision(const LocalRef&, const Revision&) override {}
    Staleness compare(const LocalRef&, const RepoListing&) const override { return Staleness{}; }
    void commit(const LocalRef&, const Revision&) override {}
    std::string cache_dir_name(const std::string& repo_id) const override { return repo_id; }
};

ArtifactRef ref(const std::string& variant) {
    ArtifactRef artifact;
    artifact.registry = RegistryId("huggingface");
    artifact.repo_id = "org/repo";
    artifact.recipe = "llamacpp";
    artifact.variant = variant;
    return artifact;
}

fs::path scratch_dir(const char* name) {
    fs::path dir = fs::temp_directory_path() /
                   (std::string("layout_") + name + "_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}

std::string read(const fs::path& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static void test_the_snapshot_directory_names_the_revision() {
    const Placement placement{"/cache/models--org--repo", "abc123", ""};
    check("the snapshot directory names the revision",
          hf_hub_layout().directory(placement) == "/cache/models--org--repo/snapshots/abc123");
    check("a placement without a revision places nothing",
          hf_hub_layout().directory(Placement{"/cache", "", ""}).empty());
    check("a repository subdirectory is kept",
          hf_hub_layout().target(placement, "assets/tokenizer.json") ==
              "/cache/models--org--repo/snapshots/abc123/assets/tokenizer.json");
    check("a name that is already absolute is refused",
          hf_hub_layout().target(placement, "/etc/passwd").empty());
}

static void test_activating_records_the_revision() {
    const fs::path cache = scratch_dir("activate");
    hf_hub_layout().activate(Placement{cache.string(), "rev-1", ""});
    check("activating writes refs/main", read(cache / "refs" / "main") == "rev-1");

    hf_hub_layout().activate(Placement{cache.string(), "", ""});
    check("an unknown revision overwrites nothing", read(cache / "refs" / "main") == "rev-1");
    fs::remove_all(cache);
}

static void test_a_sibling_lands_beside_the_file_it_belongs_to() {
    const Placement placement{"/cache", "rev", "/models/whisper.bin"};
    check("the directory is the sibling's own", sibling_layout().directory(placement) == "/models");
    check("the target sits beside the sibling",
          sibling_layout().target(placement, "cache.rai") == "/models/cache.rai");
    check("only the final component is placed, so a name cannot climb out",
          sibling_layout().target(placement, "../escape.rai") == "/models/escape.rai");
    check("with nothing to be a sibling of, nothing is placed",
          sibling_layout().directory(Placement{"/cache", "rev", ""}).empty());
    check("an empty name is refused", sibling_layout().target(placement, "").empty());

    const fs::path cache = scratch_dir("sibling");
    sibling_layout().activate(Placement{cache.string(), "rev", (cache / "model.bin").string()});
    check("a sibling cache has no revision to make current",
          !fs::exists(cache / "refs" / "main"));
    fs::remove_all(cache);
}

static void test_layout_selection_is_a_backend_fact() {
    ModelInfo info;
    info.model_name = "user.whisper";
    info.recipe = "whisper.cpp";
    info.checkpoints["main"] = "org/repo:model.bin";
    info.checkpoints["npu_cache"] = "org/repo:model.bin.rai";

    const ArtifactSourceResolver unhooked;
    check("a checkpoint with no backend fact uses the snapshot layout",
          &unhooked.layout_for(info, "main") == &hf_hub_layout());

    std::string seen_recipe;
    std::string seen_type;
    ArtifactSourceResolver::Hooks hooks;
    hooks.checkpoint_layout = [&seen_recipe, &seen_type](const std::string& recipe,
                                                         const CheckpointRef& checkpoint) {
        seen_recipe = recipe;
        seen_type = checkpoint.checkpoint_type;
        return checkpoint.checkpoint_type == "npu_cache" ? &sibling_layout() : nullptr;
    };
    const ArtifactSourceResolver resolver(hooks);

    check("the backend is asked about the checkpoint it owns",
          &resolver.layout_for(info, "npu_cache") == &sibling_layout() && seen_recipe == "whisper.cpp" &&
          seen_type == "npu_cache");
    check("a checkpoint the backend says nothing about keeps the snapshot layout",
          &resolver.layout_for(info, "main") == &hf_hub_layout());
}

bool planned(const DownloadPlan& plan, const std::string& name) {
    for (const auto& file : plan.files) {
        if (file.name == name) {
            return true;
        }
    }
    return false;
}

static void test_the_plan_names_the_files_the_ref_needs() {
    const ListingRepository repository(ref("model-Q4_K_M.gguf"));
    const RepoListing listing = repository.list("org/repo", "");
    const Placement placement{"/cache/models--org--repo", "snapshot-1", ""};

    const DownloadPlan plan =
        registry_download_plan(repository, ref("model-Q4_K_M.gguf"), hf_hub_layout(), placement,
                               listing);

    check("the plan stages into the active snapshot",
          plan.download_path == "/cache/models--org--repo/snapshots/snapshot-1");
    check("the variant's file is planned", planned(plan, "model-Q4_K_M.gguf"));
    check("a quantization the ref did not ask for is not", !planned(plan, "model-Q8_0.gguf"));
    check("the shared config a repository serves rides along", planned(plan, "config.json"));
    check("a directory entry is never a file to fetch", !planned(plan, "assets"));
    check("the url carries the revision the host's endpoint wants",
          plan.files[0].url == "https://host.example/org/repo/resolve/rev-sha/model-Q4_K_M.gguf");
    check("the size and digest come from the listing",
          plan.files[0].size == 100 && plan.files[0].expected_hash_algorithm == "sha256" &&
          plan.files[0].expected_hash == "beef");
}

static void test_a_variant_matching_no_file_is_an_error() {
    const ListingRepository repository(ref("missing.gguf"));
    const RepoListing listing = repository.list("org/repo", "");
    bool threw = false;
    try {
        registry_download_plan(repository, ref("missing.gguf"), hf_hub_layout(),
                               Placement{"/cache", "snapshot-1", ""}, listing);
    } catch (const std::exception&) {
        threw = true;
    }
    check("a variant matching no file is reported, not planned as nothing", threw);
}

}  // namespace

int main() {
    test_the_snapshot_directory_names_the_revision();
    test_activating_records_the_revision();
    test_a_sibling_lands_beside_the_file_it_belongs_to();
    test_layout_selection_is_a_backend_fact();
    test_the_plan_names_the_files_the_ref_needs();
    test_a_variant_matching_no_file_is_an_error();

    if (g_failures != 0) {
        std::printf("FAILED: %d\n", g_failures);
        return 1;
    }
    std::printf("All artifact layout checks passed\n");
    return 0;
}
