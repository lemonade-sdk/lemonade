// A registry model whose files were found in extra_models_dir: the update check
// compares against that copy, and an update lands beside it or fails in words,
// never quietly in the managed cache.

#include <lemon/model_manager.h>
#include <lemon/models/artifact_source.h>
#include <lemon/models/artifact_store.h>
#include <lemon/models/backend_availability.h>
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
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
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

// A machine that cannot run `missing_recipe`, so the hardware filter hides its models.
lemon::models::BackendAvailability machine_without(const std::string& missing_recipe) {
    lemon::models::BackendAvailability machine;
    machine.system_ram_gb = 32.0;
    machine.largest_mem_pool_gb = 24.0;
    machine.recipes[missing_recipe].unsupported_reason = "not on this test machine";
    return machine;
}

std::unique_ptr<ModelManager> make_manager(
    const Fixture& fx, const std::string& extra_dir,
    std::optional<lemon::models::BackendAvailability> machine = std::nullopt) {
    ModelManagerConfig config;
    config.extra_models_dir = extra_dir;
    if (machine) {
        config.availability = [machine](const std::map<std::string, lemon::ModelInfo>&) {
            return *machine;
        };
    }
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

std::vector<std::string> stray_extra_models(ModelManager& mm) {
    mm.invalidate_models_cache();
    std::vector<std::string> stray;
    for (const auto& [name, info] : mm.get_downloaded_models()) {
        if (info.source == "extra_models_dir") stray.push_back(name);
    }
    for (const auto& name : stray) std::printf("       stray model: %s\n", name.c_str());
    return stray;
}

// Registers more user models beside the fixture's, before a manager reads them.
void add_user_models(const Fixture& fx, const json& more) {
    json user_models = json::parse(read_file(fx.root / "user_models.json"));
    user_models.update(more);
    write_file(fx.root / "user_models.json", user_models.dump(2));
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
        PACL original = nullptr;
        if (GetNamedSecurityInfoW(dir_.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, &original, nullptr,
                                  &original_descriptor_) != ERROR_SUCCESS) {
            return;
        }
        original_dacl_ = original;
        SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
        if (!AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0,
                                      &everyone_)) {
            return;
        }
        EXPLICIT_ACCESSW deny{};
        deny.grfAccessPermissions = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY;
        deny.grfAccessMode = DENY_ACCESS;
        deny.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        deny.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        deny.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        deny.Trustee.ptstrName = static_cast<LPWSTR>(everyone_);
        PACL denied = nullptr;
        if (SetEntriesInAclW(1, &deny, original_dacl_, &denied) != ERROR_SUCCESS) return;
        ok_ = SetNamedSecurityInfoW(const_cast<LPWSTR>(dir_.c_str()), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr, nullptr, denied,
                                    nullptr) == ERROR_SUCCESS;
        LocalFree(denied);
#else
        ok_ = ::geteuid() != 0 && ::chmod(dir_.c_str(), 0555) == 0;
#endif
    }
    ~WriteDenied() {
#ifdef _WIN32
        if (ok_) {
            SetNamedSecurityInfoW(const_cast<LPWSTR>(dir_.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, original_dacl_,
                                  nullptr);
        }
        if (everyone_ != nullptr) FreeSid(everyone_);
        if (original_descriptor_ != nullptr) LocalFree(original_descriptor_);
#else
        ::chmod(dir_.c_str(), 0755);
#endif
    }
    WriteDenied(const WriteDenied&) = delete;
    WriteDenied& operator=(const WriteDenied&) = delete;
    bool ok() const { return ok_; }

  private:
    fs::path dir_;
    bool ok_ = false;
#ifdef _WIN32
    PSECURITY_DESCRIPTOR original_descriptor_ = nullptr;
    PACL original_dacl_ = nullptr;
    PSID everyone_ = nullptr;
#endif
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
    check(!fs::exists(extra_repo / "snapshots" / "snap-1"),
          "the previous snapshot is removed once the new one is in place");
    check(stray_extra_models(*mm).empty(),
          "no file of the previous version is rediscovered as an extra model");
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
#ifdef _WIN32
    check(fs::exists(old_file), "Windows keeps a replaced file a running model holds open");
#else
    check(!fs::exists(old_file), "a replaced file is unlinked even while open");
#endif
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
    check(!fs::exists(managed_repo / "snapshots" / "snap-1"),
          "the previous managed snapshot is removed too");
    check(!fs::exists(extra_repo / "snapshots" / "snap-2") &&
              read_file(extra_repo / "snapshots" / "snap-1" / "model.gguf") == gguf("old-extra"),
          "the extra copy is not touched");
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

    mm->download_model("user.Extra-Upd", json::object(), false, nullptr,
                       lemon::utils::CancelToken(), false);
    check(read_file(fx.managed / repo_dir_name("vendor/repo") / "snapshots" / "snap-2" /
                    "model.gguf") == gguf("new-main"),
          "a first pull still goes to the managed cache");
    check(!fs::exists(fx.extra / repo_dir_name("vendor/repo")),
          "a first pull writes nothing to extra_models_dir");
}

void test_an_auxiliary_revision_change_lands_beside_its_extra_copy() {
    Fixture fx = make_fixture("extra_multi");
    const fs::path main_repo =
        make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    const fs::path proj_repo =
        make_hf_repo(fx.extra, "vendor/proj", "proj-1", "proj.gguf", gguf("old-proj"));
    auto mm = make_manager(fx, path_to_utf8(fx.extra));
    check(mm->get_model_info("user.Extra-Multi").downloaded,
          "a two-repository model is adopted from extra_models_dir");

    // Both repositories moved. The registry cannot be asked whether the auxiliary
    // bytes changed, so the pull re-downloads them rather than failing.
    check_synced(*mm, "Extra-Multi", "the two-repository update succeeds");
    check(read_file(main_repo / "snapshots" / "snap-2" / "model.gguf") == gguf("new-main"),
          "the main repository updates in extra_models_dir");
    check(read_file(proj_repo / "snapshots" / "proj-2" / "proj.gguf") == gguf("new-proj") &&
              read_file(proj_repo / "refs" / "main") == "proj-2",
          "the auxiliary repository updates in extra_models_dir");
    check(fs::exists(proj_repo / ".lemonade_registry.json"),
          "the auxiliary provenance is recorded beside its extra copy");
    check(!fs::exists(fx.managed / repo_dir_name("vendor/repo")) &&
              !fs::exists(fx.managed / repo_dir_name("vendor/proj")),
          "neither repository is written to the managed cache");
    const auto info = mm->get_model_info("user.Extra-Multi");
    check(info.downloaded &&
              same_file(info.resolved_path("main"), main_repo / "snapshots" / "snap-2" / "model.gguf") &&
              same_file(info.resolved_path("mmproj"), proj_repo / "snapshots" / "proj-2" / "proj.gguf"),
          "both checkpoints resolve to their updated extra copies");
    check(!fs::exists(main_repo / "snapshots" / "snap-1") &&
              !fs::exists(proj_repo / "snapshots" / "proj-1"),
          "both repositories drop their previous snapshot");
}

// Older downloads kept an auxiliary file inside the main repository, so its own
// repository directory does not exist in extra_models_dir yet.
fs::path make_legacy_multi(const Fixture& fx) {
    const fs::path main_repo =
        make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    write_file(main_repo / "snapshots" / "snap-1" / "proj.gguf", gguf("old-proj"));
    return main_repo;
}

void test_a_legacy_auxiliary_file_updates_into_its_own_extra_repository() {
    Fixture fx = make_fixture("extra_legacy");
    const fs::path main_repo = make_legacy_multi(fx);
    const fs::path proj_repo = fx.extra / repo_dir_name("vendor/proj");
    auto mm = make_manager(fx, path_to_utf8(fx.extra));
    const auto before = mm->get_model_info("user.Extra-Multi");
    check(before.downloaded &&
              same_file(before.resolved_path("mmproj"), main_repo / "snapshots" / "snap-1" / "proj.gguf"),
          "a legacy auxiliary file is adopted from the main extra repository");
    check(!fs::exists(proj_repo), "the auxiliary repository has no extra directory yet");

    check_synced(*mm, "Extra-Multi", "the legacy-layout update succeeds");
    check(read_file(proj_repo / "snapshots" / "proj-2" / "proj.gguf") == gguf("new-proj") &&
              read_file(proj_repo / "refs" / "main") == "proj-2",
          "the auxiliary update creates its own repository in extra_models_dir");
    check(read_file(main_repo / "snapshots" / "snap-2" / "model.gguf") == gguf("new-main"),
          "the main repository updates in extra_models_dir");
    check(!fs::exists(fx.managed / repo_dir_name("vendor/repo")) &&
              !fs::exists(fx.managed / repo_dir_name("vendor/proj")),
          "neither repository is written to the managed cache");
    check(!has_probe_leftovers(proj_repo), "the write probe leaves nothing behind");
    const auto after = mm->get_model_info("user.Extra-Multi");
    check(after.downloaded &&
              same_file(after.resolved_path("mmproj"), proj_repo / "snapshots" / "proj-2" / "proj.gguf"),
          "the auxiliary checkpoint resolves to its new repository");
    check(!fs::exists(main_repo / "snapshots" / "snap-1"),
          "the legacy auxiliary file goes with the main repository's previous snapshot");
    check(stray_extra_models(*mm).empty(),
          "the legacy auxiliary file is not rediscovered as an extra model");
}

void test_a_legacy_auxiliary_directory_that_cannot_be_created_fails() {
    Fixture fx = make_fixture("extra_legacy_readonly");
    const fs::path main_repo = make_legacy_multi(fx);
    auto mm = make_manager(fx, path_to_utf8(fx.extra));

    json result;
    {
        WriteDenied deny(fx.extra);
        if (!deny.ok()) {
            std::printf("[SKIP] cannot deny writes here (running as root)\n");
            return;
        }
        result = sync(*mm, "user.Extra-Multi");
    }

    std::string why;
    check(sync_failed(result, "Extra-Multi", &why),
          "an extra root that refuses a new repository fails the update");
    std::printf("       reason: %s\n", why.c_str());
    check(why.find("extra_models_dir") != std::string::npos,
          "the failure names extra_models_dir");
    check(!fs::exists(fx.extra / repo_dir_name("vendor/proj")),
          "no auxiliary repository is created");
    check(!fs::exists(fx.managed / repo_dir_name("vendor/repo")) &&
              !fs::exists(fx.managed / repo_dir_name("vendor/proj")),
          "nothing is written to the managed cache");
    const auto info = mm->get_model_info("user.Extra-Multi");
    check(info.downloaded &&
              same_file(info.resolved_path("mmproj"), main_repo / "snapshots" / "snap-1" / "proj.gguf"),
          "the model keeps resolving to its legacy extra copy");
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

void test_a_file_another_model_still_reads_is_kept() {
    Fixture fx = make_fixture("extra_shared");
    const fs::path extra_repo =
        make_hf_repo(fx.extra, "vendor/repo", "snap-1", "model.gguf", gguf("old"));
    const fs::path old_snapshot = extra_repo / "snapshots" / "snap-1";
    write_file(old_snapshot / "other.gguf", gguf("other"));
    // Extra-Other reads a file only the old snapshot has. Extra-Twin reads the same
    // file as the model being updated, so it moves to the new copy with it.
    add_user_models(fx, {
        {"Extra-Other", {{"checkpoint", "vendor/repo:other.gguf"}, {"recipe", "llamacpp"}}},
        {"Extra-Twin", {{"checkpoint", "vendor/repo:model.gguf"}, {"recipe", "llamacpp"}}},
    });
    auto mm = make_manager(fx, path_to_utf8(fx.extra));
    check(mm->get_model_info("user.Extra-Other").downloaded &&
              mm->get_model_info("user.Extra-Twin").downloaded,
          "both neighbours are adopted from the shared extra repository");

    check_synced(*mm, "Extra-Upd", "updating one model of a shared repository succeeds");
    const fs::path updated = extra_repo / "snapshots" / "snap-2" / "model.gguf";
    check(!fs::exists(old_snapshot / "model.gguf"),
          "the replaced file goes, since every model reading it now has the new copy");
    check(read_file(old_snapshot / "other.gguf") == gguf("other"),
          "a file another model still reads is kept");
    const auto other = mm->get_model_info("user.Extra-Other");
    check(other.downloaded && same_file(other.resolved_path(), old_snapshot / "other.gguf"),
          "that model still resolves to it");
    const auto twin = mm->get_model_info("user.Extra-Twin");
    check(twin.downloaded && same_file(twin.resolved_path(), updated),
          "the model sharing the replaced file resolves to the new copy");
    check(stray_extra_models(*mm).empty(), "nothing is listed as a stray extra model");
}

void test_a_linked_blob_is_freed() {
    // The layout huggingface-cli writes: snapshot entries link into blobs/.
    Fixture fx = make_fixture("extra_blobs");
    const fs::path extra_repo = fx.extra / repo_dir_name("vendor/repo");
    const fs::path blob = extra_repo / "blobs" / "0123abcd";
    write_file(blob, gguf("old"));
    write_file(extra_repo / "refs" / "main", "snap-1");
    fs::create_directories(extra_repo / "snapshots" / "snap-1");
    std::error_code ec;
    fs::create_symlink(fs::path("..") / ".." / "blobs" / "0123abcd",
                       extra_repo / "snapshots" / "snap-1" / "model.gguf", ec);
    if (ec) {
        std::printf("[SKIP] cannot create symlinks here (%s)\n", ec.message().c_str());
        return;
    }
    auto mm = make_manager(fx, path_to_utf8(fx.extra));
    check(mm->get_model_info("user.Extra-Upd").downloaded,
          "a model whose snapshot links into blobs/ is adopted");

    check_synced(*mm, "Extra-Upd", "updating a blob-linked copy succeeds");
    check(!fs::exists(extra_repo / "snapshots" / "snap-1"), "the old snapshot link is removed");
    check(!fs::exists(blob), "and the blob behind it is freed");
}

// ---- remove_superseded_files on its own, with the model set spelled out ----

struct CleanupFixture {
    fs::path root;
    fs::path managed;
    fs::path repo;
    std::unique_ptr<lemon::models::ModelArtifactStore> store;
};

CleanupFixture make_cleanup_fixture(const std::string& prefix) {
    CleanupFixture fx;
    fx.root = lemon::test::make_scratch_dir(prefix);
    fx.managed = fx.root / "hf";
    fx.repo = fx.managed / repo_dir_name("vendor/repo");
    write_file(fx.repo / "refs" / "main", "snap-2");
    const std::string managed = path_to_utf8(fx.managed);
    fx.store = std::make_unique<lemon::models::ModelArtifactStore>(
        lemon::models::ModelArtifactStore::Hooks{
            [managed] { return managed; },
            [](const lemon::ModelInfo&, const std::string&)
                -> std::unique_ptr<lemon::models::ArtifactSource> { return nullptr; },
            [] { return std::string(); },
        });
    return fx;
}

// A model whose resolved paths are given, so the cleanup takes them as they are.
lemon::ModelInfo reading(const fs::path& path) {
    lemon::ModelInfo info;
    info.recipe = "llamacpp";
    info.source = "local_path";
    info.checkpoints["main"] = path_to_utf8(path);
    info.resolved_paths["main"] = path_to_utf8(path);
    return info;
}

bool link(const fs::path& target, const fs::path& at) {
    fs::create_directories(at.parent_path());
    std::error_code ec;
    fs::create_symlink(target, at, ec);
    if (ec) std::printf("[SKIP] cannot create symlinks here (%s)\n", ec.message().c_str());
    return !ec;
}

void test_a_directory_checkpoint_loses_its_dropped_files() {
    CleanupFixture fx = make_cleanup_fixture("cleanup_dir");
    const fs::path old_dir = fx.repo / "snapshots" / "snap-1" / "weights";
    const fs::path new_dir = fx.repo / "snapshots" / "snap-2" / "weights";
    write_file(old_dir / "dropped.onnx", "old");
    write_file(old_dir / "kept-name.onnx", "old");
    write_file(new_dir / "kept-name.onnx", "new");
    write_file(new_dir / "added.onnx", "new");

    const auto report = fx.store->remove_superseded_files(
        {old_dir / "kept-name.onnx", old_dir}, {{"Model", reading(new_dir)}});
    check(!fs::exists(old_dir / "dropped.onnx"),
          "a file the new version dropped from a directory checkpoint is removed");
    check(!fs::exists(fx.repo / "snapshots" / "snap-1"), "and the emptied snapshot goes with it");
    check(fs::exists(new_dir / "kept-name.onnx") && fs::exists(new_dir / "added.onnx"),
          "the new directory is untouched");
    check(report.removed == 2, "both old files are counted");
}

void test_a_hidden_model_keeps_its_file_through_an_update() {
    Fixture fx = make_fixture("extra_hidden");
    const fs::path main_repo = make_legacy_multi(fx);
    const fs::path legacy_proj = main_repo / "snapshots" / "snap-1" / "proj.gguf";
    // A model the hardware filter hides here, reading the legacy auxiliary file that
    // the update moves Extra-Multi away from.
    add_user_models(fx, {
        {"Extra-Hidden", {{"checkpoint", "vendor/repo:proj.gguf"}, {"recipe", "gufo"}}},
    });
    auto mm = make_manager(fx, path_to_utf8(fx.extra), machine_without("gufo"));
    check(mm->get_supported_models().count("user.Extra-Hidden") == 0 &&
              mm->get_supported_models().count("Extra-Hidden") == 0,
          "the gufo model is hidden on this declared machine");

    check_synced(*mm, "Extra-Multi", "the update beside a hidden model succeeds");
    check(read_file(legacy_proj) == gguf("old-proj"),
          "a file only a hidden model still reads is kept");
    check(!fs::exists(main_repo / "snapshots" / "snap-1" / "model.gguf"),
          "while the replaced file beside it goes");
}

void test_a_link_out_of_the_repository_loses_only_the_entry() {
    CleanupFixture fx = make_cleanup_fixture("cleanup_outside_link");
    const fs::path shared = fx.root / "shared" / "weights-v1.onnx";
    write_file(shared, "someone else's weights");
    const fs::path old_entry = fx.repo / "snapshots" / "snap-1" / "weights" / "weights-old.onnx";
    if (!link(shared, old_entry)) return;
    const fs::path new_dir = fx.repo / "snapshots" / "snap-2" / "weights";
    write_file(new_dir / "weights-new.onnx", "new");

    const auto report = fx.store->remove_superseded_files(
        {old_entry.parent_path()}, {{"Model", reading(new_dir)}});
    check(!fs::exists(fx.repo / "snapshots" / "snap-1"),
          "the superseded entry linking out of the repository is removed");
    check(read_file(shared) == "someone else's weights",
          "the file it pointed at, outside the repository's blobs, is left alone");
    check(report.bytes == 0, "and nothing is counted as freed for it");
}

void test_the_active_snapshot_is_never_cleaned() {
    CleanupFixture fx = make_cleanup_fixture("cleanup_active");
    const fs::path active = fx.repo / "snapshots" / "snap-2";
    write_file(active / "model.gguf", gguf("new"));
    write_file(active / "config.json", "{}");  // claimed by no checkpoint path

    const auto report = fx.store->remove_superseded_files(
        {active, active / "config.json"}, {{"Model", reading(active / "model.gguf")}});
    check(fs::exists(active / "config.json") && fs::exists(active / "model.gguf") &&
              report.removed == 0,
          "nothing in the snapshot refs/main names is removed, claimed or not");
}

void test_an_unresolved_local_model_is_protected() {
    CleanupFixture fx = make_cleanup_fixture("cleanup_hidden");
    const fs::path old_snapshot = fx.repo / "snapshots" / "snap-1";
    write_file(old_snapshot / "model.gguf", gguf("old"));
    write_file(old_snapshot / "shared.gguf", gguf("shared"));
    write_file(fx.repo / "snapshots" / "snap-2" / "model.gguf", gguf("new"));

    // What the hardware filter hands back: checkpoints, never resolved. A local
    // model names its file directly, so only resolving it finds the claim.
    lemon::ModelInfo hidden;
    hidden.recipe = "llamacpp";
    hidden.source = "local_path";
    hidden.checkpoints["main"] = path_to_utf8(old_snapshot / "shared.gguf");

    const auto report = fx.store->remove_superseded_files(
        {old_snapshot / "model.gguf", old_snapshot / "shared.gguf"},
        {{"Hidden", hidden},
         {"Model", reading(fx.repo / "snapshots" / "snap-2" / "model.gguf")}});
    check(read_file(old_snapshot / "shared.gguf") == gguf("shared"),
          "a file a hidden model reads is kept");
    check(!fs::exists(old_snapshot / "model.gguf"), "while the replaced file beside it goes");
    check(report.kept_in_use == 1 && report.removed == 1, "the report says which was which");
}

void test_a_file_reached_through_a_local_link_is_kept() {
    CleanupFixture fx = make_cleanup_fixture("cleanup_alias");
    const fs::path old_file = fx.repo / "snapshots" / "snap-1" / "model.gguf";
    write_file(old_file, gguf("old"));
    write_file(fx.repo / "snapshots" / "snap-2" / "model.gguf", gguf("new"));
    const fs::path alias = fx.root / "local" / "alias.gguf";
    if (!link(old_file, alias)) return;

    fx.store->remove_superseded_files(
        {old_file}, {{"Local", reading(alias)},
                     {"Model", reading(fx.repo / "snapshots" / "snap-2" / "model.gguf")}});
    check(read_file(alias) == gguf("old"), "a local model's link into the old snapshot still reads");
}

void test_a_link_chain_through_a_snapshot_keeps_entry_and_blob() {
    CleanupFixture fx = make_cleanup_fixture("cleanup_chain");
    const fs::path blob = fx.repo / "blobs" / "aaaa";
    write_file(blob, gguf("old"));
    const fs::path old_entry = fx.repo / "snapshots" / "snap-1" / "model.gguf";
    if (!link(fs::path("..") / ".." / "blobs" / "aaaa", old_entry)) return;
    write_file(fx.repo / "snapshots" / "snap-2" / "model.gguf", gguf("new"));
    const fs::path alias = fx.root / "local" / "alias.gguf";
    if (!link(old_entry, alias)) return;

    fx.store->remove_superseded_files(
        {old_entry}, {{"Local", reading(alias)},
                      {"Model", reading(fx.repo / "snapshots" / "snap-2" / "model.gguf")}});
    check(fs::is_symlink(old_entry) && fs::exists(blob) && read_file(alias) == gguf("old"),
          "a local link through a snapshot entry keeps the entry and its blob");
}

void test_snapshot_links_to_one_blob_stay_separately_removable() {
    CleanupFixture fx = make_cleanup_fixture("cleanup_shared_blob");
    const fs::path blob = fx.repo / "blobs" / "bbbb";
    write_file(blob, gguf("same"));
    const fs::path old_entry = fx.repo / "snapshots" / "snap-1" / "model.gguf";
    const fs::path new_entry = fx.repo / "snapshots" / "snap-2" / "model.gguf";
    if (!link(fs::path("..") / ".." / "blobs" / "bbbb", old_entry)) return;
    if (!link(fs::path("..") / ".." / "blobs" / "bbbb", new_entry)) return;

    fx.store->remove_superseded_files({old_entry}, {{"Model", reading(new_entry)}});
    check(!fs::exists(fx.repo / "snapshots" / "snap-1"),
          "the old snapshot's link goes, though it shares a blob with the new one");
    check(read_file(new_entry) == gguf("same"), "the blob stays for the new link");
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
        {"multi-repo", test_an_auxiliary_revision_change_lands_beside_its_extra_copy},
        {"legacy-aux", test_a_legacy_auxiliary_file_updates_into_its_own_extra_repository},
        {"legacy-aux-readonly", test_a_legacy_auxiliary_directory_that_cannot_be_created_fails},
        {"unusual-path", test_an_unusual_extra_path_is_handled},
        {"shared-repo", test_a_file_another_model_still_reads_is_kept},
        {"blob-link", test_a_linked_blob_is_freed},
        {"cleanup-directory", test_a_directory_checkpoint_loses_its_dropped_files},
        {"hidden-model", test_a_hidden_model_keeps_its_file_through_an_update},
        {"cleanup-unresolved-local", test_an_unresolved_local_model_is_protected},
        {"cleanup-active", test_the_active_snapshot_is_never_cleaned},
        {"cleanup-outside-link", test_a_link_out_of_the_repository_loses_only_the_entry},
        {"cleanup-alias", test_a_file_reached_through_a_local_link_is_kept},
        {"cleanup-alias-chain", test_a_link_chain_through_a_snapshot_keeps_entry_and_blob},
        {"cleanup-shared-blob", test_snapshot_links_to_one_blob_stay_separately_removable},
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
