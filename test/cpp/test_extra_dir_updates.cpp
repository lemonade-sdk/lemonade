// A registry model whose files were found in extra_models_dir: the update check
// compares against that copy, and an update lands beside it or fails in words,
// never quietly in the managed cache.

#include <lemon/model_manager.h>
#include <lemon/models/fetchable_repository.h>
#include <lemon/models/registry_types.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>

#include "support/test_state_isolation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::models::Acquisition;
using lemon::models::ArtifactKind;
using lemon::models::ArtifactRef;
using lemon::models::Deletion;
using lemon::models::FetchableRepository;
using lemon::models::Presence;
using lemon::models::RegistryFile;
using lemon::models::RegistryId;
using lemon::models::RepoListing;
using lemon::models::RepoRef;
using lemon::models::Revision;
using lemon::models::SourcePlacement;
using lemon::models::Staleness;
using lemon::utils::path_from_utf8;
using lemon::utils::path_to_utf8;
using json = nlohmann::json;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

std::string gguf(const std::string& marker) {
    return "GGUF" + marker + std::string(2048, 'x');
}

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string repo_dir_name(const std::string& repo_id) {
    return lemon::registry_repo_cache_dir_name(repo_id, RegistryId("huggingface"));
}

// An HF hub checkout: <root>/models--<org>--<repo>/{refs/main,snapshots/<id>/<file>}
fs::path make_hf_repo(const fs::path& root, const std::string& repo_id,
                      const std::string& snapshot, const std::string& file,
                      const std::string& bytes) {
    const fs::path repo = root / repo_dir_name(repo_id);
    write_file(repo / "refs" / "main", snapshot);
    write_file(repo / "snapshots" / snapshot / file, bytes);
    return repo;
}

bool same_file(const std::string& a, const fs::path& b) {
    std::error_code ec;
    return !a.empty() && fs::equivalent(path_from_utf8(a), b, ec);
}

bool has_probe_leftovers(const fs::path& dir) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().filename().string().rfind(".lemonade-write-probe-", 0) == 0) return true;
    }
    return false;
}

struct Remote {
    std::map<std::string, std::string> snapshot_by_repo;
    std::map<std::string, std::string> file_by_repo;
};

// Lists one file per repository at a scripted snapshot and serves it from a staged
// tree over file://, so a sync really writes bytes.
class ScriptedRepository : public FetchableRepository {
  public:
    ScriptedRepository(const ArtifactRef& ref, const Remote* remote, std::string stage)
        : FetchableRepository(ref, SourcePlacement{}), remote_(remote), stage_(std::move(stage)) {}

    ArtifactKind kind() const override { return ArtifactKind::Registry; }
    std::string resolve() const override { return ""; }
    Presence present() const override { return {}; }
    Acquisition acquisition() const override { return {}; }
    Deletion deletion() const override { return Deletion{Deletion::Kind::CacheDir, ""}; }
    RegistryId id() const override { return ref_.registry.value_or(RegistryId("huggingface")); }
    Revision default_revision() const override { return {"main", false}; }

    RepoListing list(const std::string& repo_id, const std::string& revision) const override {
        if (!revision.empty()) {
            // No older listing: the "cannot prove unchanged" branch of both the scan
            // and the pull, which is what an unreachable registry gives too.
            throw std::runtime_error("the stub has no listing for " + revision);
        }
        RepoListing listing;
        listing.repo_id = repo_id;
        listing.revision = {remote_->snapshot_by_repo.at(repo_id), false};
        listing.download_revision = "main";
        RegistryFile file;
        file.path = remote_->file_by_repo.at(repo_id);
        file.size = 0;
        listing.files = {file};
        return listing;
    }

    std::string file_url(const RepoRef& ref) const override {
        std::string flat = ref.repo_id;
        std::replace(flat.begin(), flat.end(), '/', '-');
        return "file://" + stage_ + "/" + flat + "/" + ref.path;
    }

    bool can_go_stale() const override { return true; }
    Revision active_revision(const lemon::models::LocalRef&) const override { return {}; }
    void set_active_revision(const lemon::models::LocalRef&, const Revision&) override {}
    Staleness compare(const lemon::models::LocalRef&, const RepoListing&) const override {
        return {};
    }
    void commit(const lemon::models::LocalRef&, const Revision&) override {}
    std::string cache_dir_name(const std::string& repo_id) const override {
        return repo_dir_name(repo_id);
    }

  private:
    const Remote* remote_;
    std::string stage_;
};

struct Fixture {
    fs::path root;
    fs::path managed;
    fs::path extra;
    fs::path stage;
    Remote remote;
};

// One scratch tree per case. `extra_leaf` names the extra directory, so a case can
// put spaces and non-ASCII characters in it.
Fixture make_fixture(const std::string& prefix, const std::string& extra_leaf = "extra") {
    Fixture fx;
    fx.root = lemon::test::make_scratch_dir(prefix);
    fx.managed = fx.root / "hf";
    fx.extra = fx.root / path_from_utf8(extra_leaf);
    fx.stage = fx.root / "stage";
    fs::create_directories(fx.managed);
    fs::create_directories(fx.extra);

    write_file(fx.stage / "vendor-repo" / "model.gguf", gguf("new-main"));
    write_file(fx.stage / "vendor-proj" / "proj.gguf", gguf("new-proj"));
    fx.remote.file_by_repo = {{"vendor/repo", "model.gguf"}, {"vendor/proj", "proj.gguf"}};
    fx.remote.snapshot_by_repo = {{"vendor/repo", "snap-2"}, {"vendor/proj", "proj-2"}};

    json user_models = {
        {"Extra-Upd", {{"checkpoint", "vendor/repo:model.gguf"}, {"recipe", "llamacpp"}}},
        {"Extra-Multi",
         {{"checkpoints", {{"main", "vendor/repo:model.gguf"}, {"mmproj", "vendor/proj:proj.gguf"}}},
          {"recipe", "llamacpp"}}},
    };
    write_file(fx.root / "user_models.json", user_models.dump(2));

    lemon::utils::set_cache_dir(path_to_utf8(fx.root));
    lemon::utils::set_config_dir(path_to_utf8(fx.root));
    lemon::utils::set_models_dir(path_to_utf8(fx.managed));
    return fx;
}

std::unique_ptr<ModelManager> make_manager(const Fixture& fx, const std::string& extra_dir) {
    ModelManagerConfig config;
    config.extra_models_dir = extra_dir;
    config.default_model_source = [] { return std::string("huggingface"); };
    const Remote* remote = &fx.remote;
    const std::string stage = fx.stage.generic_string();
    config.repository_for = [remote, stage](const ArtifactRef& ref) {
        return std::make_unique<ScriptedRepository>(ref, remote, stage);
    };
    return std::make_unique<ModelManager>(std::move(config));
}

json sync(ModelManager& mm, const std::string& name) {
    return mm.sync_models({name}, false, false);
}

bool sync_failed(const json& result, const std::string& name, std::string* why);

// Runs the update the way the Update button does and states whether it succeeded,
// printing the sync result when it did not.
void check_synced(ModelManager& mm, const std::string& name, const std::string& what) {
    const json result = sync(mm, "user." + name);
    const bool failed = sync_failed(result, name, nullptr);
    check(!failed && result.value("status", "") == "success", what);
    if (failed || result.value("status", "") != "success") {
        std::printf("       sync result: %s\n", result.dump().c_str());
    }
}

bool sync_failed(const json& result, const std::string& name, std::string* why = nullptr) {
    const json failed = result.value("failed_models", json::object());
    for (const auto& key : {name, std::string("user.") + name}) {
        if (failed.contains(key)) {
            if (why) *why = failed.at(key).get<std::string>();
            return true;
        }
    }
    return false;
}

// Denies everyone the right to add files under `dir` and lifts it again on
// destruction. A real ACL, because Windows ignores the read-only attribute on
// directories and the probe must see what the server would see.
class WriteDenied {
  public:
    explicit WriteDenied(fs::path dir) : dir_(std::move(dir)) {
#ifdef _WIN32
        ok_ = run("icacls \"" + dir_.string() + "\" /deny *S-1-1-0:(OI)(CI)(WD,AD) >nul 2>&1");
#else
        ok_ = ::chmod(dir_.c_str(), 0555) == 0 && ::geteuid() != 0;
#endif
    }
    ~WriteDenied() {
#ifdef _WIN32
        run("icacls \"" + dir_.string() + "\" /remove:d *S-1-1-0 >nul 2>&1");
#else
        ::chmod(dir_.c_str(), 0755);
#endif
    }
    bool ok() const { return ok_; }

  private:
    static bool run(const std::string& command) { return std::system(command.c_str()) == 0; }
    fs::path dir_;
    bool ok_ = false;
};

// ---------------------------------------------------------------------------

void test_an_extra_copy_at_the_latest_snapshot_is_up_to_date() {
    Fixture fx = make_fixture("extra_current");
    fx.remote.snapshot_by_repo["vendor/repo"] = "snap-1";
    make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    auto mm = make_manager(fx, path_to_utf8(fx.extra));

    check(mm->get_model_info("user.Extra-Upd").downloaded,
          "a registry model found only in extra_models_dir counts as downloaded");
    const auto result = mm->check_updates({"user.Extra-Upd"});
    check(std::find(result.up_to_date_models.begin(), result.up_to_date_models.end(),
                    "Extra-Upd") != result.up_to_date_models.end(),
          "an extra copy at the latest snapshot is reported up to date");
    check(!mm->get_model_info("user.Extra-Upd").update_available,
          "an extra copy at the latest snapshot carries no update flag");
}

void test_an_older_extra_copy_is_flagged() {
    Fixture fx = make_fixture("extra_stale");
    make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    auto mm = make_manager(fx, path_to_utf8(fx.extra));

    mm->check_updates({"user.Extra-Upd"});
    check(mm->get_model_info("user.Extra-Upd").update_available,
          "an extra copy behind the registry is flagged for update");
}

void test_an_update_lands_beside_the_extra_copy() {
    Fixture fx = make_fixture("extra_update");
    const fs::path extra_repo =
        make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    auto mm = make_manager(fx, path_to_utf8(fx.extra));

    check_synced(*mm, "Extra-Upd", "the in-place update succeeds");

    const fs::path updated = extra_repo / "snapshots" / "snap-2" / "model.gguf";
    check(read_file(updated) == gguf("new-main"), "the new snapshot lands in extra_models_dir");
    check(read_file(extra_repo / "refs" / "main") == "snap-2",
          "refs/main in extra_models_dir moves to the new snapshot");
    check(!fs::exists(fx.managed / repo_dir_name("vendor/repo")),
          "nothing is written to the managed cache");
    check(fs::exists(extra_repo / "snapshots" / "snap-1" / "model.gguf"),
          "the previous snapshot is kept, as the managed cache keeps it");
    check(!has_probe_leftovers(extra_repo), "the write probe leaves nothing behind");

    const auto info = mm->get_model_info("user.Extra-Upd");
    check(info.downloaded && same_file(info.resolved_path(), updated),
          "the model resolves to the updated extra copy");

    const auto again = mm->check_updates({"user.Extra-Upd"});
    check(std::find(again.up_to_date_models.begin(), again.up_to_date_models.end(),
                    "Extra-Upd") != again.up_to_date_models.end(),
          "after the update the extra copy is reported up to date");
}

void test_an_unwritable_extra_copy_fails_and_stays_put() {
    Fixture fx = make_fixture("extra_readonly");
    const fs::path extra_repo =
        make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    auto mm = make_manager(fx, path_to_utf8(fx.extra));

    json result;
    {
        WriteDenied deny(extra_repo);
        if (!deny.ok()) {
            std::printf("[SKIP] cannot deny writes here (root or no icacls)\n");
            return;
        }
        result = sync(*mm, "user.Extra-Upd");
    }

    std::string why;
    check(sync_failed(result, "Extra-Upd", &why), "an unwritable extra copy fails the update");
    std::printf("       reason: %s\n", why.c_str());
    check(why.find("extra_models_dir") != std::string::npos &&
              why.find(path_to_utf8(extra_repo)) != std::string::npos,
          "the failure names extra_models_dir and the directory");
    check(!fs::exists(fx.managed / repo_dir_name("vendor/repo")),
          "a refused update writes nothing to the managed cache");
    check(!fs::exists(extra_repo / "snapshots" / "snap-2"), "and creates no new snapshot");
    check(read_file(extra_repo / "refs" / "main") == "snap-1", "refs/main is untouched");
    check(!has_probe_leftovers(extra_repo), "a refused probe leaves nothing behind");
    const auto info = mm->get_model_info("user.Extra-Upd");
    check(info.downloaded && same_file(info.resolved_path(),
                                       extra_repo / "snapshots" / "snap-1" / "model.gguf"),
          "the model keeps resolving to its existing extra copy");
}

void test_an_open_old_file_does_not_block_the_update() {
    Fixture fx = make_fixture("extra_in_use");
    const fs::path extra_repo =
        make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    auto mm = make_manager(fx, path_to_utf8(fx.extra));
    const fs::path old_file = extra_repo / "snapshots" / "snap-1" / "model.gguf";

    std::string why;
    json result;
    {
#ifdef _WIN32
        // A loaded llama-server holds the GGUF open with read sharing only.
        HANDLE held = CreateFileW(old_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        check(held != INVALID_HANDLE_VALUE, "the old file is held open like a loaded model");
#else
        std::ifstream held(old_file, std::ios::binary);
#endif
        result = sync(*mm, "user.Extra-Upd");
#ifdef _WIN32
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
#endif
    }
    check(!sync_failed(result, "Extra-Upd", &why) && result.value("status", "") == "success",
          "an update succeeds while the old file is open");
    if (!why.empty()) std::printf("       reason: %s\n", why.c_str());
    check(read_file(extra_repo / "snapshots" / "snap-2" / "model.gguf") == gguf("new-main"),
          "the new snapshot lands beside the open one");
    check(fs::exists(old_file), "the open old file is left alone");
}

void test_a_managed_copy_still_wins() {
    Fixture fx = make_fixture("extra_both");
    const fs::path extra_repo =
        make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old-extra"));
    const fs::path managed_repo =
        make_hf_repo(fx.managed, "vendor/repo", "snap-1", "model.gguf", gguf("old-managed"));
    auto mm = make_manager(fx, path_to_utf8(fx.extra));

    check(same_file(mm->get_model_info("user.Extra-Upd").resolved_path(),
                    managed_repo / "snapshots" / "snap-1" / "model.gguf"),
          "a complete managed copy is the one resolved");
    check_synced(*mm, "Extra-Upd", "updating the managed copy succeeds");
    check(read_file(managed_repo / "snapshots" / "snap-2" / "model.gguf") == gguf("new-main"),
          "the update lands in the managed cache");
    check(!fs::exists(extra_repo / "snapshots" / "snap-2"), "the extra copy is not touched");
}

void test_an_extra_root_containing_the_managed_cache_is_not_misread() {
    Fixture fx = make_fixture("extra_parent");
    const fs::path managed_repo =
        make_hf_repo(fx.managed, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    // The extra root is the managed cache's parent, so every managed path is inside it.
    auto mm = make_manager(fx, path_to_utf8(fx.root));

    check_synced(*mm, "Extra-Upd", "updating a managed copy under the extra root succeeds");
    check(read_file(managed_repo / "snapshots" / "snap-2" / "model.gguf") == gguf("new-main"),
          "the update stays in the managed cache");
    check(!fs::exists(fx.root / repo_dir_name("vendor/repo")),
          "no repository is invented at the extra root");
}

void test_a_model_on_no_disk_is_pulled_into_the_managed_cache() {
    Fixture fx = make_fixture("extra_fresh");
    auto mm = make_manager(fx, path_to_utf8(fx.extra));

    mm->download_model("user.Extra-Upd", json::object());
    check(read_file(fx.managed / repo_dir_name("vendor/repo") / "snapshots" / "snap-2" /
                    "model.gguf") == gguf("new-main"),
          "a first pull still goes to the managed cache");
    check(!fs::exists(fx.extra / repo_dir_name("vendor/repo")),
          "a first pull writes nothing to extra_models_dir");
}

void test_an_auxiliary_repository_stays_beside_its_extra_copy() {
    Fixture fx = make_fixture("extra_multi");
    // The auxiliary snapshot stays put: a moved one makes the pull ask the HF tree
    // API for metadata, which no stub answers. The pull still writes refs and
    // provenance into the auxiliary repository, so its directory choice is tested.
    fx.remote.snapshot_by_repo["vendor/proj"] = "proj-1";
    const fs::path main_repo =
        make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    const fs::path proj_repo =
        make_hf_repo(fx.extra, "vendor/proj", "proj-1", "proj.gguf", gguf("old-proj"));
    auto mm = make_manager(fx, path_to_utf8(fx.extra));
    check(mm->get_model_info("user.Extra-Multi").downloaded,
          "a two-repository model is adopted from extra_models_dir");

    check_synced(*mm, "Extra-Multi", "the two-repository update succeeds");
    check(read_file(main_repo / "snapshots" / "snap-2" / "model.gguf") == gguf("new-main"),
          "the main repository updates in extra_models_dir");
    check(read_file(proj_repo / "snapshots" / "proj-1" / "proj.gguf") == gguf("old-proj") &&
              read_file(proj_repo / "refs" / "main") == "proj-1",
          "the unchanged auxiliary repository keeps its extra copy");
    check(fs::exists(proj_repo / ".lemonade_registry.json"),
          "the auxiliary provenance is recorded beside its extra copy");
    check(!fs::exists(fx.managed / repo_dir_name("vendor/repo")) &&
              !fs::exists(fx.managed / repo_dir_name("vendor/proj")),
          "neither repository is written to the managed cache");
    const auto info = mm->get_model_info("user.Extra-Multi");
    check(info.downloaded &&
              same_file(info.resolved_path("main"), main_repo / "snapshots" / "snap-2" / "model.gguf") &&
              same_file(info.resolved_path("mmproj"), proj_repo / "snapshots" / "proj-1" / "proj.gguf"),
          "both checkpoints resolve to their extra copies after the update");
}

void test_an_unusual_extra_path_is_handled() {
    // Spaces and non-ASCII characters, and on Windows a configured path whose case
    // differs from the directory on disk.
    Fixture fx = make_fixture("extra_unicode", u8"Extra Modèles");
    const fs::path extra_repo =
        make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    std::string configured = path_to_utf8(fx.extra);
#ifdef _WIN32
    std::transform(configured.begin(), configured.end(), configured.begin(), [](char c) {
        return static_cast<unsigned char>(c) < 0x80
                   ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                   : c;
    });
#endif
    auto mm = make_manager(fx, configured);
    check(mm->get_model_info("user.Extra-Upd").downloaded,
          "a model under an unusual extra path is adopted");

    check_synced(*mm, "Extra-Upd", "the update under an unusual extra path succeeds");
    check(read_file(extra_repo / "snapshots" / "snap-2" / "model.gguf") == gguf("new-main"),
          "the update lands under the unusual extra path");
    check(!fs::exists(fx.managed / repo_dir_name("vendor/repo")),
          "nothing is written to the managed cache");
}

}  // namespace

int main() {
    // Nothing listens here, so a metadata comparison fails fast and the code under
    // test takes its "assume changed" branch instead of reaching a real host.
    set_env_var("HF_ENDPOINT", "http://127.0.0.1:9");

    const std::vector<std::pair<const char*, void (*)()>> cases = {
        {"current", test_an_extra_copy_at_the_latest_snapshot_is_up_to_date},
        {"stale", test_an_older_extra_copy_is_flagged},
        {"update", test_an_update_lands_beside_the_extra_copy},
        {"readonly", test_an_unwritable_extra_copy_fails_and_stays_put},
        {"in-use", test_an_open_old_file_does_not_block_the_update},
        {"managed-wins", test_a_managed_copy_still_wins},
        {"extra-parent", test_an_extra_root_containing_the_managed_cache_is_not_misread},
        {"fresh", test_a_model_on_no_disk_is_pulled_into_the_managed_cache},
        {"multi-repo", test_an_auxiliary_repository_stays_beside_its_extra_copy},
        {"unusual-path", test_an_unusual_extra_path_is_handled},
    };
    for (const auto& [name, run] : cases) {
        std::printf("\n=== %s ===\n", name);
        try {
            run();
        } catch (const std::exception& e) {
            check(false, std::string(name) + " threw: " + e.what());
        }
    }

    std::printf("\n%s\n", failures == 0 ? "ALL PASSED" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
