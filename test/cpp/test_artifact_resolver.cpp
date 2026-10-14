#include "lemon/model_manager.h"
#include "lemon/models/artifact_resolver.h"
#include "lemon/models/fetchable_repository.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

using lemon::ModelInfo;
using lemon::models::ArtifactKind;
using lemon::models::ArtifactRef;
using lemon::models::ArtifactSource;
using lemon::models::ArtifactSourceResolver;
using lemon::models::Acquisition;
using lemon::models::Deletion;
using lemon::models::FetchableRepository;
using lemon::models::LocalRef;
using lemon::models::Presence;
using lemon::models::RegistrationError;
using lemon::models::RegistryId;
using lemon::models::RepoListing;
using lemon::models::RepoRef;
using lemon::models::Revision;
using lemon::models::Staleness;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

class SpyRepository : public FetchableRepository {
  public:
    explicit SpyRepository(ArtifactRef ref) : FetchableRepository(std::move(ref)) {}

    ArtifactKind kind() const override { return ArtifactKind::Registry; }
    std::string resolve() const override { return ""; }
    Presence present() const override { return Presence{}; }
    Acquisition acquisition() const override { return Acquisition{}; }
    Deletion deletion() const override { return Deletion{}; }

    RegistryId id() const override { return RegistryId("zzhost"); }
    Revision default_revision() const override { return Revision{"trunk", false}; }
    RepoListing list(const std::string& repo_id, const std::string&) const override {
        return RepoListing{repo_id, Revision{}, {}};
    }
    std::string file_url(const RepoRef& ref) const override { return ref.path; }
    bool can_go_stale() const override { return true; }
    Revision active_revision(const LocalRef&) const override { return Revision{}; }
    void set_active_revision(const LocalRef&, const Revision&) override {}
    Staleness compare(const LocalRef&, const RepoListing&) const override { return Staleness{}; }
    void commit(const LocalRef&, const Revision&) override {}
    std::string cache_dir_name(const std::string& repo_id) const override { return repo_id; }
};

RegistryId g_built_host;
ArtifactRef g_built_ref;

ArtifactSourceResolver::Hooks base_hooks() {
    ArtifactSourceResolver::Hooks hooks;
    hooks.fetches_no_artifacts = [](const std::string& recipe) { return recipe == "cloud"; };
    hooks.backend_self_manages_downloads =
        [](const std::string& recipe) { return recipe == "flm"; };
    hooks.repository_for = [](const RegistryId& id, const ArtifactRef& ref)
        -> std::unique_ptr<FetchableRepository> {
        g_built_host = id;
        g_built_ref = ref;
        return std::make_unique<SpyRepository>(ref);
    };
    return hooks;
}

ModelInfo model(const std::string& recipe, const std::string& checkpoint,
                const std::string& source = "", const std::string& registry_source = "") {
    ModelInfo info;
    info.model_name = "test.model";
    info.recipe = recipe;
    info.checkpoints["main"] = checkpoint;
    info.source = source;
    info.registry_source = registry_source;
    return info;
}

std::string temp_file() {
    namespace fs = std::filesystem;
    fs::path path = fs::temp_directory_path() /
                    ("resolver_" + std::to_string(
                                       std::chrono::steady_clock::now().time_since_epoch().count()) +
                     ".gguf");
    std::ofstream(path) << "bytes";
    return path.string();
}

}  // namespace

static void test_a_cloud_entry_has_no_bytes() {
    const ArtifactSourceResolver resolver(base_hooks());
    const std::unique_ptr<ArtifactSource> source =
        resolver.source_for(model("cloud", "someone/somewhere"), "main");

    check("a cloud route has no artifact kind", source->kind() == ArtifactKind::None);
    check("and reports itself downloaded", source->present().complete);
    check("nothing is fetched",
          source->acquisition().kind == Acquisition::Kind::NotApplicable);
    check("and nothing is deleted", source->deletion().kind == Deletion::Kind::Nothing);
}

static void test_a_backend_that_owns_its_weights_delegates_both() {
    const ArtifactSourceResolver resolver(base_hooks());
    const std::unique_ptr<ArtifactSource> source =
        resolver.source_for(model("flm", "gemma3:4b"), "main");

    check("a self-managed recipe is its own kind",
          source->kind() == ArtifactKind::SelfManaged);
    const Acquisition acquisition = source->acquisition();
    check("the fetch is delegated", acquisition.kind == Acquisition::Kind::Delegated);
    check("and the text names who fetches it", acquisition.delegate == "flm");
    check("so removal is delegated too",
          source->deletion().kind == Deletion::Kind::Delegate);
}

static void test_the_origin_decides_whose_bytes_they_are() {
    const ArtifactSourceResolver resolver(base_hooks());

    const std::unique_ptr<ArtifactSource> placed =
        resolver.source_for(model("llamacpp", "/models/thing.gguf", "local_path"), "main");
    check("a placed file is local", placed->kind() == ArtifactKind::Local);
    check("and deleting the model leaves it",
          placed->deletion().kind == Deletion::Kind::Nothing);

    const std::unique_ptr<ArtifactSource> scanned =
        resolver.source_for(model("llamacpp", "/models/thing.gguf", "extra_models_dir"), "main");
    check("a scanned file is left too",
          scanned->deletion().kind == Deletion::Kind::Nothing);

    const std::unique_ptr<ArtifactSource> uploaded =
        resolver.source_for(model("llamacpp", "/cache/upload.gguf", "local_upload"), "main");
    check("but a copy lemond wrote is lemond's to remove",
          uploaded->deletion().kind == Deletion::Kind::CacheDir);
    check("and it resolves to the recorded copy", uploaded->resolve() == "/cache/upload.gguf");

    // A Windows absolute path holds a colon; only the source keeps it from being
    // read as a repository named "C" with a variant.
    const std::unique_ptr<ArtifactSource> windows =
        resolver.source_for(model("llamacpp", "C:\\models\\thing.gguf", "local_path"), "main");
    check("a windows path stays one file", windows->kind() == ArtifactKind::Local);
    check("and its colon is not read as a variant",
          windows->resolve() == "C:\\models\\thing.gguf");
}

static void test_an_explicit_scheme_outranks_the_default_host() {
    const ArtifactSourceResolver resolver(base_hooks());
    ModelInfo info = model("llamacpp", "hf://someone/somewhere");
    info.registry_source = "modelscope";

    const std::unique_ptr<ArtifactSource> source = resolver.source_for(info, "main");
    check("the scheme in the checkpoint wins",
          g_built_host.str() == "huggingface" && source->kind() == ArtifactKind::Registry);
    check("and is stripped from the repository it names",
          g_built_ref.repo_id == "someone/somewhere");
}

static void test_the_host_of_a_repository_comes_from_the_entry_then_the_config() {
    ArtifactSourceResolver::Hooks hooks = base_hooks();
    hooks.default_registry_id = [] { return std::string("modelscope"); };
    const ArtifactSourceResolver resolver(hooks);

    resolver.source_for(model("llamacpp", "someone/somewhere:fp16", "", "modelscope"), "main");
    check("a pinned registry is honored", g_built_host.str() == "modelscope");
    check("the variant is carried on the ref", g_built_ref.variant == "fp16");

    resolver.source_for(model("llamacpp", "someone/somewhere"), "main");
    check("an unpinned entry takes the configured default",
          g_built_host.str() == "modelscope");
}

static void test_no_host_and_no_default_is_an_error_not_a_miss() {
    const ArtifactSourceResolver resolver(base_hooks());
    bool threw = false;
    try {
        resolver.source_for(model("llamacpp", "someone/somewhere"), "main");
    } catch (const RegistrationError&) {
        threw = true;
    }
    check("an unpinned entry with no default registry reports a registration error", threw);
}

static void test_presence_asks_the_backend_that_reads_the_file() {
    const std::string path = temp_file();
    ArtifactSourceResolver::Hooks accepting = base_hooks();
    accepting.validate_file = [](const std::string&, const std::string&) { return std::string(); };
    const ArtifactSourceResolver resolver(accepting);

    ModelInfo info = model("llamacpp", path, "local_path");
    check("a file that exists is present", resolver.source_for(info, "main")->present().complete);

    ArtifactSourceResolver::Hooks rejecting = base_hooks();
    rejecting.validate_file = [](const std::string&, const std::string&) {
        return std::string("no gguf magic");
    };
    const ArtifactSourceResolver strict(rejecting);
    const Presence presence = strict.source_for(info, "main")->present();
    check("a file the backend rejects is not present", !presence.complete);
    check("and says why", presence.reason == "no gguf magic");

    std::filesystem::remove(path);
}

static void test_a_lazily_acquired_checkpoint_is_not_required() {
    const ArtifactSourceResolver resolver(base_hooks());
    ModelInfo info = model("flm", "gemma3:4b");
    info.checkpoints["npu_cache"] = "someone/compiled:rai";

    check("an ordinary checkpoint is required",
          resolver.source_for(info, "main")->ref().required);
    check("a lazily acquired one is not",
          !resolver.source_for(info, "npu_cache")->ref().required);
}

static void test_a_checkpoint_that_binds_to_nothing_is_reported() {
    const ArtifactSourceResolver resolver(base_hooks());
    bool threw = false;
    try {
        resolver.source_for(model("llamacpp", "notarepo"), "main");
    } catch (const RegistrationError&) {
        threw = true;
    }
    check("a name that is neither repository nor file is a registration error", threw);
}

static void test_a_file_on_disk_outranks_the_repo_id_heuristic() {
    const std::string path = temp_file();
    const ArtifactSourceResolver resolver(base_hooks());
    const std::unique_ptr<ArtifactSource> source =
        resolver.source_for(model("llamacpp", path), "main");

    check("a path that exists is a file, though it carries a slash",
          source->kind() == ArtifactKind::Local);
    check("and resolves to itself", source->resolve() == path);

    std::filesystem::remove(path);
}

static void test_a_missing_org_repo_still_binds_to_its_host() {
    ArtifactSourceResolver::Hooks hooks = base_hooks();
    hooks.default_registry_id = [] { return std::string("huggingface"); };
    const ArtifactSourceResolver resolver(hooks);

    const std::unique_ptr<ArtifactSource> source =
        resolver.source_for(model("llamacpp", "someone/somewhere"), "main");
    check("bytes that are not here are fetched, not reported missing",
          source->kind() == ArtifactKind::Registry);

    std::filesystem::remove(temp_file());
}

int main() {
    test_a_cloud_entry_has_no_bytes();
    test_a_backend_that_owns_its_weights_delegates_both();
    test_the_origin_decides_whose_bytes_they_are();
    test_an_explicit_scheme_outranks_the_default_host();
    test_the_host_of_a_repository_comes_from_the_entry_then_the_config();
    test_no_host_and_no_default_is_an_error_not_a_miss();
    test_presence_asks_the_backend_that_reads_the_file();
    test_a_lazily_acquired_checkpoint_is_not_required();
    test_a_checkpoint_that_binds_to_nothing_is_reported();
    test_a_file_on_disk_outranks_the_repo_id_heuristic();
    test_a_missing_org_repo_still_binds_to_its_host();

    if (g_failures == 0) {
        std::printf("All artifact resolver tests passed.\n");
        return 0;
    }
    std::printf("%d artifact resolver test(s) failed.\n", g_failures);
    return 1;
}
