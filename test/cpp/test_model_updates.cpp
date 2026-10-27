// One binary, the suites below run in order in one process. Each keeps its scratch
// directories and its own failure count; the only shared state is the process-wide
// models directory, which every suite sets on entry.

#include <lemon/model_manager.h>
#include <lemon/models/registry_types.h>
#include <lemon/models/fetchable_repository.h>
#include <lemon/models/registry_id.h>
#include <lemon/utils/path_utils.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "show_all_models.h"
#include <lemon/models/artifact_resolver.h>
#include <lemon/models/update_scan.h>
#include <lemon/models/artifact_source.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/runtime_config.h>
#include <ctime>
#include <memory>

// The flag the desktop badges a model with. No gate read `update_available` before
// this file: a scan that settled every determination and then published nothing would
// pass every suite. These drive a real ModelManager over fixture bytes and a stub
// registry, then read the flag back off the row a reader asks for.
namespace available {

// The flag the desktop badges a model with. No gate read `update_available` before
// this file: a scan that settled every determination and then published nothing would
// pass every suite. These drive a real ModelManager over fixture bytes and a stub
// registry, then read the flag back off the row a reader asks for.




namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::ModelRegistry;
using lemon::RegistryFile;
using lemon::RegistryId;
using lemon::RegistryRepository;
using lemon::json;
using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool asked_about(const std::vector<std::string>& asked, const std::string& repo_id) {
    for (const auto& entry : asked) {
        if (entry == repo_id || entry.rfind(repo_id + "@", 0) == 0) return true;
    }
    return false;
}

void write_file(const fs::path& path, const std::string& contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << contents;
}

// An HF snapshot checkout: <models>/models--<org>--<repo>/{refs/main,snapshots/<id>/<files>}
void make_hf_repo(const fs::path& models_dir,
                  const std::string& repo,
                  const std::string& snapshot,
                  const std::vector<std::string>& files) {
    std::string slug = "models--";
    for (char c : repo) {
        if (c == '/') slug += "--";
        else slug += c;
    }
    const fs::path repo_dir = models_dir / slug;
    write_file(repo_dir / "refs" / "main", snapshot);
    for (const auto& file : files) {
        write_file(repo_dir / "snapshots" / snapshot / file, "GGUF");
    }
}

// One source's answers, remembering what it was asked. `unreachable` and `fail_repo`
// fail the way a down registry does, so the failure paths belong to the fixture.
struct StubState {
    mutable std::vector<std::string> asked;
    std::string latest_snapshot = "snap-new";
    std::map<std::string, std::string> latest_by_repo;
    std::string fail_repo;
    bool unreachable = false;
    bool can_go_stale = true;
};

class StubRepository : public lemon::models::FetchableRepository {
  public:
    StubRepository(const lemon::models::ArtifactRef& ref, StubState* state)
        : FetchableRepository(ref, lemon::models::SourcePlacement{}), state_(state) {}

    lemon::models::ArtifactKind kind() const override {
        return lemon::models::ArtifactKind::Registry;
    }
    std::string resolve() const override { return ""; }
    lemon::models::Presence present() const override { return {}; }
    lemon::models::Acquisition acquisition() const override { return {}; }
    lemon::models::Deletion deletion() const override {
        return lemon::models::Deletion{lemon::models::Deletion::Kind::CacheDir, ""};
    }

    lemon::models::RegistryId id() const override {
        return ref_.registry.value_or(lemon::models::huggingface_descriptor().id);
    }
    lemon::models::Revision default_revision() const override { return {"main", false}; }

    lemon::models::RepoListing list(const std::string& repo_id,
                                    const std::string& revision) const override {
        state_->asked.push_back(revision.empty() ? repo_id : repo_id + "@" + revision);
        if (state_->unreachable) throw std::runtime_error("the stub registry is unreachable");
        if (!state_->fail_repo.empty() && state_->fail_repo == repo_id)
            throw std::runtime_error("the stub cannot reach " + repo_id);

        if (!revision.empty()) {
            // A registry that cannot list the revision on disk leaves the scan unable
            // to prove the files match, which is the "assume changed" branch.
            throw std::runtime_error("the stub has no listing for " + revision);
        }

        lemon::models::RepoListing listing;
        listing.repo_id = repo_id;
        const auto per_repo = state_->latest_by_repo.find(repo_id);
        listing.revision = {per_repo == state_->latest_by_repo.end() ? state_->latest_snapshot
                                                                      : per_repo->second,
                            false};
        listing.download_revision = "main";
        RegistryFile file;
        file.path = "main.gguf";
        file.size = 4;
        listing.files = {file};
        return listing;
    }

    std::string file_url(const lemon::models::RepoRef& ref) const override {
        return "https://stub.invalid/" + ref.repo_id + "/" + ref.revision.id + "/" + ref.path;
    }

    bool can_go_stale() const override { return state_->can_go_stale; }
    lemon::models::Revision active_revision(const lemon::models::LocalRef&) const override {
        return {};
    }
    void set_active_revision(const lemon::models::LocalRef&,
                             const lemon::models::Revision&) override {}
    lemon::models::Staleness compare(const lemon::models::LocalRef&,
                                     const lemon::models::RepoListing&) const override {
        return {};
    }
    void commit(const lemon::models::LocalRef&, const lemon::models::Revision&) override {}

    std::string cache_dir_name(const std::string& repo_id) const override {
        std::string slug = "models--";
        for (char c : repo_id) {
            if (c == '/') slug += "--";
            else slug += c;
        }
        return slug;
    }

  private:
    StubState* state_;
};

struct Fixture {
    fs::path root;
    fs::path models_dir;
};

Fixture make_fixture() {
    Fixture fx;
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    fx.root = fs::temp_directory_path() / ("lemonade_update_flag_" + std::to_string(tick));
    fx.models_dir = fx.root / "hf";
    fs::create_directories(fx.models_dir);

    // Every repository holds `snap-old`; the stub decides what is newer.
    make_hf_repo(fx.models_dir, "zz/updatable", "snap-old", {"main.gguf"});
    make_hf_repo(fx.models_dir, "zz/second", "snap-old", {"main.gguf"});
    make_hf_repo(fx.models_dir, "zz/mmprojrepo", "snap-old", {"proj.gguf"});

    const fs::path local_file = fx.root / "local" / "zz-local.gguf";
    write_file(local_file, "GGUF");

    json user_models = {
        {"zz-updatable", json{{"checkpoint", "zz/updatable:main.gguf"}, {"recipe", "llamacpp"}}},
        {"zz-second", json{{"checkpoint", "zz/second:main.gguf"}, {"recipe", "llamacpp"}}},
        {"zz-multi",
         json{{"checkpoints",
               json{{"main", "zz/updatable:main.gguf"},
                    {"mmproj", "zz/mmprojrepo:proj.gguf"}}},
              {"recipe", "llamacpp"}}},
        {"zz-local", json{{"checkpoints", json{{"main", path_to_utf8(local_file)}}},
                          {"recipe", "llamacpp"},
                          {"source", "local_path"}}},
    };
    write_file(fx.root / "user_models.json", user_models.dump(2));
    return fx;
}

bool flagged(ModelManager& manager, const std::string& canonical_id) {
    return manager.get_model_info(canonical_id).update_available;
}

}  // namespace

int run() {
    // Nothing listens on this port, so a scan that reached the network instead of the
    // stub would fail rather than quietly describe the real Hugging Face.
    set_env_var("HF_ENDPOINT", "http://127.0.0.1:9");

    ShowAllModels show_all_models;
    const Fixture fx = make_fixture();
    lemon::utils::set_cache_dir(path_to_utf8(fx.root));
    lemon::utils::set_config_dir(path_to_utf8(fx.root));
    lemon::utils::set_models_dir(path_to_utf8(fx.models_dir));

    StubState registry;
    ModelManagerConfig config;
    config.repository_for = [&registry](const lemon::models::ArtifactRef& ref) {
        return std::make_unique<StubRepository>(ref, &registry);
    };
    ModelManager manager(std::move(config));

    check("the fixture model starts downloaded",
          manager.get_model_info("user.zz-updatable").downloaded);
    check("a model starts with no update flagged", !flagged(manager, "user.zz-updatable"));

    // A newer snapshot, and no listing for the revision on disk: the scan cannot prove
    // the bytes it downloads are the same, so it advertises one.
    const auto newer = manager.check_updates({"zz-updatable"});
    check("a newer snapshot is reported under the public name",
          contains(newer.updated_models, "zz-updatable"));
    check("a newer snapshot reaches the row a reader asks", flagged(manager, "user.zz-updatable"));
    check("the scan asked the registry its caller named", asked_about(registry.asked, "zz/updatable"));

    // A down registry settles nothing, so the determination made above survives it.
    registry.unreachable = true;
    const auto failed = manager.check_updates({"zz-updatable"});
    check("an unreachable registry reports the model as failed",
          failed.failed_models.count("zz-updatable") == 1);
    check("a failed check reports no update", failed.updated_models.empty());
    check("a failed check neither sets nor clears the flag", flagged(manager, "user.zz-updatable"));

    // A completed determination of "unchanged" is what takes the flag back off.
    registry.unreachable = false;
    registry.latest_snapshot = "snap-old";
    const auto unchanged = manager.check_updates({"zz-updatable"});
    check("the model is reported up to date",
          contains(unchanged.up_to_date_models, "zz-updatable"));
    check("an unchanged registry clears the flag", !flagged(manager, "user.zz-updatable"));

    registry.latest_snapshot = "snap-newer";
    registry.asked.clear();
    const auto targeted = manager.check_updates({"user.zz-updatable"});
    check("a targeted scan reports its target", contains(targeted.updated_models, "zz-updatable"));
    check("a targeted scan asks nothing about the model outside it",
          !asked_about(registry.asked, "zz/second"));
    check("a model outside the target carries no flag", !flagged(manager, "user.zz-second"));

    // The main repository did settle a change, but a scan that could not finish has no
    // answer about the model as a whole: it neither announces one nor moves the flag.
    registry.fail_repo = "zz/mmprojrepo";
    registry.latest_snapshot = "snap-newer";
    registry.asked.clear();
    const auto partial = manager.check_updates({"zz-multi"});
    check("a model whose auxiliary repository failed is reported as failed",
          partial.failed_models.count("zz-multi") == 1);
    check("a model whose auxiliary repository failed is not announced as updated",
          !contains(partial.updated_models, "zz-multi"));
    check("a model whose auxiliary repository failed keeps the flag it had",
          !flagged(manager, "user.zz-multi"));
    check("both of its repositories were asked",
          asked_about(registry.asked, "zz/mmprojrepo") &&
              asked_about(registry.asked, "zz/updatable"));
    // A finished answer about one repository is still not an answer about the model.
    registry.latest_by_repo["zz/updatable"] = "snap-old";
    const auto partial_unchanged = manager.check_updates({"zz-multi"});
    check("a model whose auxiliary repository failed is not announced as up to date",
          !contains(partial_unchanged.up_to_date_models, "zz-multi"));
    check("and it is still reported as failed", partial_unchanged.failed_models.count("zz-multi") == 1);
    registry.latest_by_repo.clear();
    registry.fail_repo.clear();

    // A source that says its bytes cannot go stale is skipped before it is asked:
    // checking it could only lie.
    registry.asked.clear();
    registry.can_go_stale = false;
    const bool flag_before = flagged(manager, "user.zz-updatable");
    const auto frozen = manager.check_updates({});
    check("a source that cannot go stale is never listed", registry.asked.empty());
    check("a source that cannot go stale reports nothing", frozen.updated_models.empty() &&
          frozen.up_to_date_models.empty() && frozen.failed_models.empty());
    check("a source that cannot go stale moves no flag",
          flagged(manager, "user.zz-updatable") == flag_before);
    registry.can_go_stale = true;

    const auto every = manager.check_updates({});
    check("a local-path model is never asked about", !asked_about(registry.asked, "zz/local"));
    check("a local-path model is not reported updated", !contains(every.updated_models, "zz-local"));
    check("a local-path model is not reported up to date either",
          !contains(every.up_to_date_models, "zz-local"));
    check("a local-path model carries no flag", !flagged(manager, "user.zz-local"));
    check("the remote model beside it still settles an update",
          contains(every.updated_models, "zz-updatable"));

    if (failures == 0) {
        std::printf("All update available flag tests passed.\n");
    } else {
        std::printf("%d update available flag test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace available


namespace scan {

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::models::ArtifactSource;
using lemon::models::ScanDetermination;
using lemon::models::StoreSnapshot;
using lemon::models::UpdateScanDeps;
using lemon::models::UpdateScanResult;

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

ModelInfo registry_model(const std::string& name, const std::string& recipe) {
    ModelInfo info;
    info.model_name = name;
    info.recipe = recipe;
    info.downloaded = true;
    info.checkpoints["main"] = "lemonade-tests/no-such-repo";
    return info;
}

// Every eligibility rule the scan applies, one model each.
std::shared_ptr<StoreSnapshot> make_store(const std::string& public_name) {
    auto snapshot = std::make_shared<StoreSnapshot>();
    snapshot->valid = true;

    snapshot->models["eligible"] = registry_model("eligible", "llamacpp");
    snapshot->models["flm"] = registry_model("flm", "flm");
    snapshot->models["cloud"] = registry_model("cloud", "cloud");

    ModelInfo imported = registry_model("imported", "llamacpp");
    imported.source = "local_path";
    snapshot->models["imported"] = imported;

    ModelInfo uncheckpointed = registry_model("uncheckpointed", "llamacpp");
    uncheckpointed.checkpoints.clear();
    snapshot->models["uncheckpointed"] = uncheckpointed;

    ModelInfo absent = registry_model("absent", "llamacpp");
    absent.downloaded = false;
    snapshot->models["absent"] = absent;

    snapshot->canonical_public_names["eligible"] = public_name;
    snapshot->public_model_aliases[public_name] = "eligible";
    return snapshot;
}

struct Probe {
    std::shared_ptr<StoreSnapshot> snapshot;
    int ensure_store_calls = 0;
    bool published = false;
    ScanDetermination settled;
    std::vector<std::string> phases;
};

// An artifact store rooted at the test directory, empty as the day it was built.
struct Cache {
    explicit Cache(std::filesystem::path r) : root(std::move(r)) {}
    std::filesystem::path root;
    lemon::models::ModelArtifactStore artifacts{lemon::models::ModelArtifactStore::Hooks{
        [this] { return root.string(); },
        [](const ModelInfo&, const std::string&) -> std::unique_ptr<ArtifactSource> {
            return nullptr;
        },
        [] { return std::string(); },
    }};
};

// What the scan does with a host is what this test pins, so the host is a stub that answers
// "I can go stale" and then fails on the tree: the same shape as a registry that refuses the
// connection, which is what HF_ENDPOINT pointed at a dead port for.
class UnreachableRepository : public lemon::models::FetchableRepository {
  public:
    explicit UnreachableRepository(const lemon::models::ArtifactRef& ref)
        : FetchableRepository(ref, lemon::models::SourcePlacement{}) {}

    lemon::models::ArtifactKind kind() const override { return lemon::models::ArtifactKind::Registry; }
    std::string resolve() const override { return ""; }
    lemon::models::Presence present() const override { return {}; }
    lemon::models::Acquisition acquisition() const override { return {}; }
    lemon::models::Deletion deletion() const override {
        return lemon::models::Deletion{lemon::models::Deletion::Kind::CacheDir, ""};
    }

    lemon::models::RegistryId id() const override {
        return ref_.registry.value_or(lemon::models::huggingface_descriptor().id);
    }
    lemon::models::Revision default_revision() const override { return {"main", false}; }
    lemon::models::RepoListing list(const std::string&, const std::string&) const override {
        throw std::runtime_error("the fixture registry is unreachable");
    }
    std::string file_url(const lemon::models::RepoRef&) const override { return {}; }
    bool can_go_stale() const override { return true; }
    lemon::models::Revision active_revision(const lemon::models::LocalRef&) const override { return {}; }
    void set_active_revision(const lemon::models::LocalRef&, const lemon::models::Revision&) override {}
    lemon::models::Staleness compare(const lemon::models::LocalRef&,
                                     const lemon::models::RepoListing&) const override {
        return {};
    }
    void commit(const lemon::models::LocalRef&, const lemon::models::Revision&) override {}
    std::string cache_dir_name(const std::string& repo_id) const override {
        std::string slug = "models--";
        for (char c : repo_id) slug += (c == '/') ? "--" : std::string(1, c);
        return slug;
    }
};

UpdateScanDeps make_deps(Probe& probe, Cache& cache) {
    UpdateScanDeps deps;
    // This test is about the scan's bookkeeping, so the seam answers "no host here" and the
    // scan's own error path runs — which is what an unregistered host does in production.
    static const lemon::models::ArtifactSourceResolver stub_host{
        lemon::models::ArtifactSourceResolver::Hooks{
            {},
            [](const lemon::models::RegistryId&, const lemon::models::ArtifactRef& ref,
               const lemon::models::SourcePlacement&) {
                return std::make_unique<UnreachableRepository>(ref);
            },
            {}}};
    deps.resolver = &stub_host;
    deps.artifacts = &cache.artifacts;
    deps.ensure_store = [&probe] { ++probe.ensure_store_calls; };
    deps.snapshot = [&probe] {
        return std::const_pointer_cast<const StoreSnapshot>(probe.snapshot);
    };
    deps.cancel_requested = [] { return false; };
    deps.publish = [&probe](const ScanDetermination& settled, UpdateScanResult&) {
        probe.published = true;
        probe.settled = settled;
    };
    deps.phase = [&probe](const std::string& phase) { probe.phases.push_back(phase); };
    return deps;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

int run() {
    // Nothing listens here, so every registry round trip fails at once. The scan
    // still has to finish, report the failure and settle nothing.
    set_env_var("HF_ENDPOINT", "http://127.0.0.1:9");

    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() / ("lemonade_update_scan_test_" +
                                                        std::to_string(tick));
    std::error_code ec;
    fs::create_directories(base, ec);

    {
        lemon::RuntimeConfig offline(lemon::json{{"offline", true}});
        lemon::RuntimeConfig::set_global(&offline);

        Probe probe;
        probe.snapshot = make_store("eligible");
        Cache cache(base);
        UpdateScanDeps deps = make_deps(probe, cache);
        const UpdateScanResult result = lemon::models::run_update_scan(deps);

        check("offline scan reports nothing", result.updated_models.empty() &&
                                                  result.up_to_date_models.empty() &&
                                                  result.failed_models.empty());
        check("offline scan touches neither the store nor the publish",
              probe.ensure_store_calls == 0 && !probe.published);
        lemon::RuntimeConfig::set_global(nullptr);
    }

    {
        Probe probe;
        probe.snapshot = make_store("Shown-Name");
        Cache cache(base);
        UpdateScanDeps deps = make_deps(probe, cache);
        const UpdateScanResult result = lemon::models::run_update_scan(deps);

        check("only a downloaded registry checkpoint is examined",
              probe.settled.examined.size() == 1 &&
                  probe.settled.examined[0] == "eligible");
        check("an unreachable registry settles no staleness", probe.settled.verified.empty() &&
                                                                   probe.settled.updated.empty());
        check("a failed check is reported under the listed name",
              result.failed_models.count("Shown-Name") != 0);
        check("the phase hook names the model being checked",
              contains(probe.phases, "registry_check:eligible"));
        check("the scan publishes once per pass", probe.published);
    }

    {
        Probe probe;
        probe.snapshot = make_store("eligible");
        Cache cache(base);
        UpdateScanDeps deps = make_deps(probe, cache);
        const UpdateScanResult result = lemon::models::run_update_scan(deps, {"flm"});

        check("a target outside the eligible set examines nothing",
              probe.settled.examined.empty() && result.failed_models.empty());
    }

    {
        Probe probe;
        probe.snapshot = make_store("eligible");
        Cache cache(base);
        UpdateScanDeps deps = make_deps(probe, cache);
        deps.scan_override = [](const std::vector<std::string>&) {
            UpdateScanResult res;
            res.updated_models.push_back("overridden-updated");
            res.up_to_date_models.push_back("overridden-current");
            res.failed_models["overridden-failed"] = "forced";
            return res;
        };
        const UpdateScanResult result = lemon::models::run_update_scan(deps);

        check("the override result is returned verbatim",
              result.updated_models.size() == 1 && result.updated_models[0] == "overridden-updated" &&
                  result.up_to_date_models.size() == 1 &&
                  result.failed_models.count("overridden-failed") != 0);
        check("an overridden pass reads and publishes nothing",
              probe.ensure_store_calls == 0 && !probe.published);
        check("every reported model gets its phase, once",
              probe.phases.size() == 3);
    }

    fs::remove_all(base, ec);
    std::printf("%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}

}  // namespace scan

int main() {
    int failures = 0;
    std::printf("\n=== available ===\n");
    failures |= available::run();
    std::printf("\n=== scan ===\n");
    failures |= scan::run();
    return failures;
}
