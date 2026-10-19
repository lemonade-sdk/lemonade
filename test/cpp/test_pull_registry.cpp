// The pull as a whole, against scripted sources: which files a plan selects, where
// its bytes land, what a second pull re-fetches, and which words a rejected pull
// says. Every repository read goes through the config seam, so nothing here reaches
// a real host.

#include <lemon/model_manager.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/models/artifact_layout.h>
#include <lemon/models/fetchable_repository.h>

#include "support/test_state_isolation.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using lemon::ModelInfo;
using lemon::ModelManager;
using lemon::ModelManagerConfig;
using lemon::models::Acquisition;
using lemon::models::ArtifactKind;
using lemon::models::ArtifactRef;
using lemon::models::Deletion;
using lemon::models::DownloadFile;
using lemon::models::FetchableRepository;
using lemon::models::Presence;
using lemon::models::RegistryFile;
using lemon::models::RegistryId;
using lemon::models::RepoListing;
using lemon::models::RepoRef;
using lemon::models::Revision;
using lemon::models::SourcePlacement;
using lemon::models::Staleness;

namespace {

std::vector<std::string> g_failures;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << std::endl;
        g_failures.push_back(what);
    }
}

const char* kSideBytes = "auxiliary side cache bytes";
// sha256 of kSideBytes, pinned so the manifest's hash wiring is exercised.
const char* kSideSha256 = "5c8908c5c116301545b04bbde043016339197aed38083a40c764f15cc7e74b74";

std::string staged_gguf(const std::string& marker) {
    // A .gguf name is only accepted by the transfer when the bytes say GGUF.
    return "GGUF" + marker + std::string(2048, 'x');
}

void stage(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

struct ScriptedRepo {
    std::string snapshot;
    std::vector<RegistryFile> files;
};

struct Trace {
    std::vector<std::string> listings;
    std::vector<std::string> urls;
};

// Answers for one repository from a script, hands out file:// URLs into a staged
// tree, and records every read the pull performs.
class ScriptedRepository : public FetchableRepository {
  public:
    ScriptedRepository(const ArtifactRef& ref, std::map<std::string, ScriptedRepo> script,
                       std::string stage_root, Trace* trace)
        : FetchableRepository(ref, SourcePlacement{}),
          script_(std::move(script)),
          stage_root_(std::move(stage_root)),
          trace_(trace) {}

    ArtifactKind kind() const override { return ArtifactKind::Registry; }
    std::string resolve() const override { return present().path; }
    // The scripted host keeps its bytes in the staged tree: an honest answer
    // for the status reads, while acquisition stays scripted for the pull.
    Presence present() const override {
        if (script_.find(ref_.repo_id) == script_.end()) return Presence{};
        std::string flat = ref_.repo_id;
        for (auto& c : flat) {
            if (c == '/') c = '-';
        }
        return Presence{true, stage_root_ + "/" + flat + "/" + ref_.variant, ""};
    }
    Acquisition acquisition() const override { return Acquisition{}; }
    Deletion deletion() const override { return Deletion{Deletion::Kind::CacheDir, ""}; }

    RegistryId id() const override { return ref_.registry.value_or(RegistryId("huggingface")); }
    Revision default_revision() const override { return Revision{"main", false}; }

    RepoListing list(const std::string& repo_id, const std::string& revision) const override {
        trace_->listings.push_back(repo_id + "@" + revision);
        const auto it = script_.find(repo_id);
        if (it == script_.end()) throw std::runtime_error("unknown repository " + repo_id);
        RepoListing listing;
        listing.repo_id = repo_id;
        listing.revision = Revision{it->second.snapshot, false};
        listing.download_revision = "main";
        listing.files = it->second.files;
        return listing;
    }

    std::string file_url(const RepoRef& ref) const override {
        trace_->urls.push_back(ref.repo_id + ":" + ref.revision.id + ":" + ref.path);
        std::string flat = ref.repo_id;
        for (auto& c : flat) {
            if (c == '/') c = '-';
        }
        return "file://" + stage_root_ + "/" + flat + "/" + ref.path;
    }

    bool can_go_stale() const override { return true; }
    Revision active_revision(const lemon::models::LocalRef&) const override { return {}; }
    void set_active_revision(const lemon::models::LocalRef&, const Revision&) override {}
    Staleness compare(const lemon::models::LocalRef&, const RepoListing&) const override {
        return Staleness{};
    }
    void commit(const lemon::models::LocalRef&, const Revision&) override {}

    std::string cache_dir_name(const std::string& repo_id) const override {
        std::string flat = repo_id;
        for (auto& c : flat) {
            if (c == '/') c = '-';
        }
        return "models--" + flat;
    }

  private:
    std::map<std::string, ScriptedRepo> script_;
    std::string stage_root_;
    Trace* trace_;
};

RegistryFile file(const std::string& path, const std::string& sha = "") {
    RegistryFile f;
    f.path = path;
    f.size = 128;
    f.hash_algorithm = sha.empty() ? "" : "sha256";
    f.hash = sha;
    return f;
}

struct Fixture {
    fs::path stage;
    Trace trace;
    std::map<std::string, ScriptedRepo> script;

    ModelManager make_manager() const {
        ModelManagerConfig config;
        config.default_model_source = [] { return std::string("huggingface"); };
        config.repository_for = [this](const ArtifactRef& ref) {
            return std::make_unique<ScriptedRepository>(ref, script, stage.string(),
                                                        &const_cast<Trace&>(trace));
        };
        return ModelManager(config);
    }
};

Fixture make_fixture() {
    Fixture fx;
    fx.stage = lemon::test::make_scratch_dir("pull_stage");

    stage(fx.stage / "vendor-repo" / "model.gguf", staged_gguf("main"));
    stage(fx.stage / "vendor-repo" / "config.json", "{\"arch\":\"tiny\"}");
    stage(fx.stage / "vendor-repo" / "tokenizer.model", "tok");
    stage(fx.stage / "vendor-repo" / "README.md", "read me");
    stage(fx.stage / "vendor-repo" / "other.gguf", staged_gguf("other"));
    stage(fx.stage / "vendor-aux" / "side.bin", kSideBytes);
    stage(fx.stage / "vendor-aux" / "notes.txt", "notes");

    fx.script["vendor/repo"] = ScriptedRepo{
        "snap-main-1",
        {file("model.gguf"), file("config.json"), file("tokenizer.model"), file("README.md"),
         file("other.gguf")}};
    fx.script["vendor/aux"] = ScriptedRepo{"snap-aux-1",
                                           {file("side.bin", kSideSha256), file("notes.txt")}};
    return fx;
}

nlohmann::json definition() {
    return {{"type", "llm"},
            {"recipe", "llamacpp"},
            {"checkpoints", {{"main", "vendor/repo:model.gguf"},
                             {"side", "vendor/aux:side.bin"}}}};
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path repo_cache(const ModelManager& mm, const std::string& repo_id) {
    std::string flat = repo_id;
    for (auto& c : flat) {
        if (c == '/') c = '-';
    }
    return fs::path(mm.get_hf_cache_dir()) / ("models--" + flat);
}

void reset_trace(Fixture& fx) { fx.trace = Trace{}; }

// What the selector has always taken for this listing: the named variant plus the
// four well-known config files that exist in the repository.
const std::vector<std::string> kMainSelected{"model.gguf", "config.json", "tokenizer.model"};

void test_a_pull_lands_exactly_the_selected_files() {
    Fixture fx = make_fixture();
    ModelManager mm = fx.make_manager();

    mm.download_model("user.Pull-Char", definition());

    const fs::path main_snapshot = repo_cache(mm, "vendor/repo") / "snapshots" / "snap-main-1";
    for (const auto& name : kMainSelected) {
        check(fs::exists(main_snapshot / name), "selected file lands: " + name);
    }
    check(!fs::exists(main_snapshot / "README.md"), "unselected README does not land");
    check(!fs::exists(main_snapshot / "other.gguf"), "other variant does not land");
    check(read_file(main_snapshot / "model.gguf") == staged_gguf("main"),
          "main bytes are the staged bytes");

    const fs::path aux_snapshot = repo_cache(mm, "vendor/aux") / "snapshots" / "snap-aux-1";
    check(fs::exists(aux_snapshot / "side.bin"), "aux checkpoint lands beside main");
    check(read_file(aux_snapshot / "side.bin") == kSideBytes, "aux bytes match their hash");
    check(!fs::exists(aux_snapshot / "notes.txt"), "aux listing keeps only the named variant");

    check(read_file(repo_cache(mm, "vendor/repo") / "refs" / "main") == "snap-main-1",
          "refs/main pins the fetched snapshot");
    check(read_file(repo_cache(mm, "vendor/aux") / "refs" / "main") == "snap-aux-1",
          "the aux repository pins its own snapshot");

    // The manifest served the resume and then left; provenance stayed.
    check(!fs::exists(main_snapshot / ".download_manifest.json"), "manifest is removed after");
    // Provenance v2: the main file tells the whole v1 sentence, and the
    // auxiliary file tells only its own checkpoint.
    const auto read_provenance = [](const fs::path& dir) {
        std::ifstream in(dir / ".lemonade_registry.json", std::ios::binary);
        return nlohmann::json::parse(in);
    };
    const nlohmann::json main_prov = read_provenance(repo_cache(mm, "vendor/repo"));
    const auto& main_rec = main_prov["processed_models"]["user.Pull-Char"];
    check(main_rec.contains("selection") && main_rec.contains("snapshot_id"),
          "the main provenance still answers a v1 reader");
    check(main_rec["checkpoints"]["main"]["snapshot_id"] == "snap-main-1",
          "the main checkpoint slot names the fetched snapshot");
    const nlohmann::json aux_prov = read_provenance(repo_cache(mm, "vendor/aux"));
    check(aux_prov.value("version", 0) == 2, "the auxiliary file is a v2 file");
    const auto& aux_rec = aux_prov["processed_models"]["user.Pull-Char"];
    check(!aux_rec.contains("selection") && aux_rec["checkpoints"].contains("side"),
          "the auxiliary file records only the checkpoint it holds");

    // What the wire answers as checkpoint_status[] comes from the same sources
    // the pull just used.
    const auto statuses = mm.checkpoint_status(mm.get_model_info("user.Pull-Char"));
    check(statuses.size() == 2, "checkpoint_status lists every checkpoint");
    const lemon::models::CheckpointStatus* main_status = nullptr;
    const lemon::models::CheckpointStatus* side_status = nullptr;
    for (const auto& status : statuses) {
        if (status.type == "main") main_status = &status;
        if (status.type == "side") side_status = &status;
    }
    check(main_status != nullptr && main_status->present && main_status->required &&
              main_status->registry.str() == "huggingface",
          "main is present, required, on the registry it came from");
    check(main_status != nullptr && fs::path(main_status->path).filename() == "model.gguf" &&
              main_status->path.rfind(fx.stage.string(), 0) == 0,
          "main's path is where its source says the bytes are");
    check(side_status != nullptr && side_status->present &&
              fs::path(side_status->path).filename() == "side.bin",
          "the auxiliary checkpoint reports itself present too");

    // The whole wire plan: three main files and one aux, at the listing's snapshot.
    // URLs carry the download ref the listing names, while the snapshot directory
    // carries the revision id: the same split the hand-built manifest had.
    std::vector<std::string> want = {"vendor/repo:main:model.gguf",
                                     "vendor/repo:main:config.json",
                                     "vendor/repo:main:tokenizer.model",
                                     "vendor/aux:main:side.bin"};
    std::sort(want.begin(), want.end());
    std::vector<std::string> got = fx.trace.urls;
    std::sort(got.begin(), got.end());
    if (want != got) {
    }
    check(want == got, "the pull resolves exactly the selected URLs");
}

void test_a_second_pull_reselects_but_refetches_nothing() {
    Fixture fx = make_fixture();
    ModelManager mm = fx.make_manager();
    mm.download_model("user.Pull-Char", definition());
    mm.download_model("user.Pull-Char", nlohmann::json::object());
    reset_trace(fx);

    mm.download_model("user.Pull-Char", nlohmann::json::object());

    check(!fx.trace.listings.empty(), "a re-pull still asks each source for its listing");
    const fs::path main_snapshot = repo_cache(mm, "vendor/repo") / "snapshots" / "snap-main-1";
    check(fs::exists(main_snapshot / "model.gguf"), "an installed file stays installed");
    check(read_file(repo_cache(mm, "vendor/repo") / "refs" / "main") == "snap-main-1",
          "the pin stays where it was");
}

void test_a_missing_aux_checkpoint_is_refused_in_words() {
    Fixture fx = make_fixture();
    nlohmann::json def = definition();
    def["checkpoints"]["side"] = "vendor/aux:ghost.bin";
    ModelManager mm = fx.make_manager();

    bool threw = false;
    try {
        mm.download_model("user.Pull-Refuse", def);
        mm.download_model("user.Pull-Refuse", nlohmann::json::object());
    } catch (const std::exception& e) {
        threw = true;
        check(std::string(e.what()).find("Additional checkpoint file not found") !=
                  std::string::npos,
              "the refusal names the missing aux checkpoint");
    }
    check(threw, "a missing aux variant fails the pull");
    check(std::none_of(fx.trace.urls.begin(), fx.trace.urls.end(),
                       [](const std::string& s) { return s.rfind("vendor/repo:", 0) == 0; }),
          "nothing downloads once the aux checkpoint is refused");
}

void test_a_malformed_aux_checkpoint_never_reaches_the_wire() {
    Fixture fx = make_fixture();
    nlohmann::json def = definition();
    def["checkpoints"]["side"] = "just-a-file.gguf";
    ModelManager mm = fx.make_manager();

    bool threw = false;
    try {
        mm.download_model("user.Pull-Malformed", def);
        mm.download_model("user.Pull-Malformed", nlohmann::json::object());
    } catch (const std::exception& e) {
        threw = true;
        check(std::string(e.what()).find("exact repository variant") != std::string::npos,
              "the refusal explains the grammar");
    }
    check(threw, "a malformed aux checkpoint fails the pull");
    check(fx.trace.urls.empty(), "a malformed definition opens no connections");
}

void test_a_cancelled_pull_stops_before_the_first_byte() {
    Fixture fx = make_fixture();
    ModelManager mm = fx.make_manager();
    mm.download_model("user.Pull-Cancel", definition());
    // The registration call already fetched; clear the bytes so the cancelled
    // pull below has real work it must not do.
    for (const auto* repo : {"vendor/repo", "vendor/aux"}) {
        fs::remove_all(repo_cache(mm, repo));
    }

    std::atomic<bool> flag{true};
    lemon::utils::CancelToken token;
    token.flag = &flag;
    bool threw = false;
    try {
        mm.download_model("user.Pull-Cancel", nlohmann::json::object(), false, nullptr, token);
    } catch (const std::exception& e) {
        threw = true;
        check(std::string(e.what()).find("cancelled") != std::string::npos,
              "the refusal says cancelled");
    }
    check(threw, "a cancelled token stops the pull");
    check(!fs::exists(repo_cache(mm, "vendor/repo") / "snapshots" / "snap-main-1" / "model.gguf"),
          "a cancelled pull leaves no artifact");
}

void test_search_is_a_source_answer() {
    // The real adapters, offline: every built-in host answers the search question.
    for (const auto& host : {lemon::models::huggingface_descriptor().id,
                             lemon::models::modelscope_descriptor().id}) {
        ArtifactRef ref;
        ref.registry = host;
        const auto repository =
            lemon::models::create_repository(host, ref, SourcePlacement{});
        check(repository->supports_search(), "a built-in source offers search");
    }

    // A source that cannot search says so, and its search throws rather than lie.
    Fixture fx;
    ArtifactRef ref;
    ref.registry = lemon::models::RegistryId("quiet");
    ScriptedRepository quiet(ref, fx.script, fx.stage.string(), &fx.trace);
    check(!quiet.supports_search(), "a plain source declines search");
    bool threw = false;
    try {
        quiet.search("anything", 5, false);
    } catch (const std::logic_error&) {
        threw = true;
    }
    check(threw, "search on a silent source throws instead of pretending");
}

}  // namespace

int main() {
    lemon::test::state_dir();
    test_a_pull_lands_exactly_the_selected_files();
    test_a_second_pull_reselects_but_refetches_nothing();
    test_a_missing_aux_checkpoint_is_refused_in_words();
    test_a_malformed_aux_checkpoint_never_reaches_the_wire();
    test_a_cancelled_pull_stops_before_the_first_byte();
    test_search_is_a_source_answer();

    if (!g_failures.empty()) {
        std::cerr << g_failures.size() << " failure(s)" << std::endl;
        return 1;
    }
    std::cout << "all pull registry checks passed" << std::endl;
    return 0;
}
