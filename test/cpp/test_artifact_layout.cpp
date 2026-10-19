#include "lemon/model_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/models/artifact_layout.h"
#include "lemon/models/artifact_resolver.h"
#include "lemon/models/fetchable_repository.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::models::ArtifactKind;
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
using lemon::models::SourcePlacement;
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
    ListingRepository(ArtifactRef ref, SourcePlacement placement = SourcePlacement{})
        : FetchableRepository(std::move(ref), placement) {}

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
            {"model.bin.rai", 50, "", "", false},
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
    artifact.variant = variant;
    return artifact;
}

// Whose policy reads the address, and so which files of the repository it wants.
SourcePlacement policy(const std::string& recipe) {
    SourcePlacement bound;
    bound.recipe = recipe;
    return bound;
}

ArtifactRef side_ref(const std::string& variant) {
    ArtifactRef artifact = ref(variant);
    artifact.checkpoint_type = "npu_cache";
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
    info.recipe = "whispercpp";
    info.checkpoints["main"] = "org/repo:model.bin";
    info.checkpoints["npu_cache"] = "org/repo:model.bin.rai";

    // The layout is the backend's fact, asked of its ops: whisper places the NPU
    // cache beside the .bin it was built for, and says nothing about the .bin.
    const ArtifactSourceResolver resolver;
    check("a side cache beside another checkpoint uses the sibling layout",
          &resolver.layout_for(info, "npu_cache") == &sibling_layout());
    check("a checkpoint the backend says nothing about keeps the snapshot layout",
          &resolver.layout_for(info, "main") == &hf_hub_layout());

    ModelInfo other = info;
    other.recipe = "llamacpp";
    check("a recipe with no layout opinion resolves everything to snapshots",
          &resolver.layout_for(other, "npu_cache") == &hf_hub_layout());
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
    const ListingRepository repository(ref("model-Q4_K_M.gguf"), policy("llamacpp"));
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
    const ListingRepository repository(ref("missing.gguf"), policy("llamacpp"));
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

static void test_a_side_checkpoint_fetches_only_its_own_file() {
    const ArtifactRef artifact = side_ref("model.bin.rai");
    const ListingRepository repository(artifact, policy("whispercpp"));
    Placement placement;
    placement.sibling = "/models/whisper.bin";
    placement.revision = "snapshot-1";

    const DownloadPlan plan =
        registry_download_plan(repository, artifact, sibling_layout(), placement,
                               repository.list("org/repo", ""));

    check("a side checkpoint lands beside the file it belongs to", plan.download_path == "/models");
    check("a side checkpoint fetches only the file it names",
          plan.files.size() == 1 && plan.files[0].name == "model.bin.rai");
    check("a side checkpoint does not collect the repository's other files",
          !planned(plan, "config.json"));
}

static void test_a_side_checkpoint_naming_an_absent_file_is_an_error() {
    const ArtifactRef artifact = side_ref("absent.rai");
    const ListingRepository repository(artifact, policy("whispercpp"));
    Placement placement;
    placement.sibling = "/models/whisper.bin";
    bool threw = false;
    try {
        registry_download_plan(repository, artifact, sibling_layout(), placement,
                               repository.list("org/repo", ""));
    } catch (const std::exception&) {
        threw = true;
    }
    check("a side checkpoint naming a file the repository lacks is reported", threw);
}

// The address says which bytes; the placement says whose policy reads them. One ref, two
// backends: a GGUF selection takes the quantization the variant names, a ThinkSound one
// wants its own fixed set of audio files and finds none of them here.
static void test_the_policy_travels_with_the_source_not_the_address() {
    const ArtifactRef artifact = ref("model-Q4_K_M.gguf");
    const ListingRepository ggufs(artifact, policy("llamacpp"));
    const ListingRepository audio(artifact, policy("thinksound"));
    const RepoListing listing = ggufs.list("org/repo", "");
    const Placement where{"/cache", "snapshot-1", ""};

    const DownloadPlan gguf_plan =
        registry_download_plan(ggufs, artifact, hf_hub_layout(), where, listing);
    const DownloadPlan audio_plan =
        registry_download_plan(audio, artifact, hf_hub_layout(), where, listing);

    check("the placement's recipe decides what the same address offers",
          planned(gguf_plan, "model-Q4_K_M.gguf") &&
          !planned(audio_plan, "model-Q4_K_M.gguf"));
    check("a policy that wants other files still takes the shared config",
          planned(audio_plan, "config.json"));
    check("choosing a policy leaves the address and its destination alone",
          gguf_plan.download_path == audio_plan.download_path);

    ArtifactRef same_bytes = artifact;
    check("identity is the address alone", same_bytes == artifact);
}

static void test_a_sibling_source_resolves_beside_its_model() {
    const fs::path model_dir = scratch_dir("sibling_source");
    const fs::path main_file = model_dir / "whisper.bin";
    std::ofstream(main_file) << "model";

    const SourcePlacement binding{&sibling_layout(), main_file.string()};
    const auto source =
        lemon::models::create_repository(RegistryId("huggingface"), side_ref("whisper.bin.rai"),
                                         binding);
    const std::string expected = (model_dir / "whisper.bin.rai").string();

    check("a sibling source resolves beside its model", source->resolve() == expected);
    check("a missing side cache is absent, with a reason naming where it looked",
          !source->present().complete && source->present().reason.find(expected) != std::string::npos);

    std::ofstream(expected) << "cache";
    check("a side cache sitting beside the model is present",
          source->present().complete && source->present().path == expected);

    fs::remove_all(model_dir);
}

static void test_the_model_layer_hands_a_source_the_backends_layout() {
    const fs::path model_dir = scratch_dir("resolver_wiring");
    const fs::path main_file = model_dir / "whisper.bin";
    std::ofstream(main_file) << "model";

    ModelManagerConfig config;
    config.default_model_source = [] { return "huggingface"; };
    ModelManager manager(std::move(config));

    ModelInfo info;
    info.model_name = "user.whisper";
    info.recipe = "whispercpp";
    info.checkpoints["main"] = main_file.string();
    info.checkpoints["npu_cache"] = "org/repo:whisper.bin.rai";
    // A store build fills these; a source built outside one has no anchor to know.
    info.resolved_paths["main"] = main_file.string();

    const auto source = manager.source_for(info, "npu_cache");
    const auto repository = dynamic_cast<const FetchableRepository*>(source.get());
    check("the side checkpoint binds to the default host's repository",
          source != nullptr && source->kind() == ArtifactKind::Registry);
    check("the source carries the layout the backend declared for that checkpoint",
          repository != nullptr && &repository->layout() == &sibling_layout());
    check("the source knows the file it must sit beside",
          repository != nullptr && repository->sibling() == main_file.string());
    check("the source knows which checkpoint it is",
          source->ref().checkpoint_type == "npu_cache" && source->ref().variant == "whisper.bin.rai");

    const auto main_source = manager.source_for(info, "main");
    check("a file on disk needs no repository", main_source->kind() == ArtifactKind::Local);

    fs::remove_all(model_dir);
}

static void test_a_sibling_plan_moves_bytes_where_the_layout_says() {
    const fs::path dir = scratch_dir("transfer");
    const fs::path staged = dir / "whisper.bin.rai";
    std::ofstream(staged) << "cache-bytes";
    const fs::path model_dir = dir / "model";
    fs::create_directories(model_dir);

    DownloadPlan plan;
    plan.download_path = sibling_layout().directory(Placement{"", "", (model_dir / "whisper.bin").string()});
    lemon::models::DownloadFile file;
    file.name = staged.filename().string();
    file.url = "file://" + staged.string();
    plan.files.push_back(file);

    lemon::models::ArtifactTransfer::with_default_transfers().execute(plan, {}, nullptr, {});

    check("a sibling plan writes into the model's own directory",
          fs::exists(model_dir / "whisper.bin.rai"));
    check("and the bytes arrive intact", read(model_dir / "whisper.bin.rai") == "cache-bytes");
    fs::remove_all(dir);
}

}  // namespace

int main() {
    // A ModelManager reads config and option files, so the run stays in a scratch
    // directory.
    const fs::path fixture = scratch_dir("fixture");
    lemon::utils::set_cache_dir(fixture.string());
    lemon::utils::set_config_dir(fixture.string());
    lemon::utils::set_models_dir((fixture / "hf").string());

    test_the_snapshot_directory_names_the_revision();
    test_activating_records_the_revision();
    test_a_sibling_lands_beside_the_file_it_belongs_to();
    test_layout_selection_is_a_backend_fact();
    test_the_plan_names_the_files_the_ref_needs();
    test_the_policy_travels_with_the_source_not_the_address();
    test_a_variant_matching_no_file_is_an_error();
    test_a_side_checkpoint_fetches_only_its_own_file();
    test_a_side_checkpoint_naming_an_absent_file_is_an_error();
    test_a_sibling_source_resolves_beside_its_model();
    test_the_model_layer_hands_a_source_the_backends_layout();
    test_a_sibling_plan_moves_bytes_where_the_layout_says();

    if (g_failures != 0) {
        std::printf("FAILED: %d\n", g_failures);
        return 1;
    }
    std::printf("All artifact layout checks passed\n");
    return 0;
}
