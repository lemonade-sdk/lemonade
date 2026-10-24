// The acceptance criteria for a third pullable host: it is added in one file, it pulls
// with and without a socket, and no component learns its name. Run here, in a test
// binary, so the registration never reaches a shipped server's model sources.
#include <lemon/models/artifact_layout.h>
#include <lemon/models/artifact_transfer.h>
#include <lemon/models/fetchable_repository.h>
#include <lemon/models/model_sources.h>
#include <lemon/models/store_snapshot.h>
#include <lemon/utils/http_client.h>
#include <lemon/utils/path_utils.h>

#include <httplib.h>

#include "support/test_state_isolation.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef LEMONADE_REPO_ROOT
#error "LEMONADE_REPO_ROOT must name the repository root"
#endif

namespace fs = std::filesystem;
using namespace lemon;
using namespace lemon::models;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_env(const char* name) {
#ifdef _WIN32
    _putenv((std::string(name) + "=").c_str());
#else
    unsetenv(name);
#endif
}

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream body;
    body << in.rdbuf();
    return body.str();
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

template <typename Action>
bool throws(const Action& action) {
    try {
        action();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

ArtifactRef side_ref(const RegistryId& host, const std::string& repo, const std::string& file) {
    ArtifactRef ref;
    ref.registry = host;
    ref.checkpoint_type = "mmproj";  // A side checkpoint names its one file.
    ref.repo_id = repo;
    ref.variant = file;
    return ref;
}

// The second FileTransfer the spec's second rule prescribes for a protocol the shipped
// transfers decline. It owns one scheme and nothing else: dispatch, disk-space
// accounting and the progress total stay in ArtifactTransfer, which is the whole point.
// The shipped HttpFileTransfer is policy-seamed, but this fixture keeps its own
// transfer to prove the registration seam stays open to a second implementation.
class LoopbackHttpTransfer : public FileTransfer {
  public:
    bool handles(const std::string& url) const override {
        return url.rfind("http://127.0.0.1:", 0) == 0;
    }

    std::uint64_t transfer(const DownloadFile& file, const std::string& target_dir,
                           const std::map<std::string, std::string>& headers,
                           const ByteProgress& progress,
                           const lemon::utils::CancelToken& cancel) const override {
        if (cancel.cancelled()) {
            throw TransferError("Download cancelled", true);
        }
        const std::string rest = file.url.substr(std::strlen("http://127.0.0.1:"));
        const std::size_t slash = rest.find('/');
        httplib::Client client("http://127.0.0.1:" + rest.substr(0, slash));
        httplib::Headers sent(headers.begin(), headers.end());
        const auto response = client.Get(rest.substr(slash).c_str(), sent);
        if (!response || response->status != 200) {
            throw TransferError("fixture hub refused " + file.name);
        }
        const std::string output = target_dir + "/" + file.name;
        std::ofstream out(utils::path_from_utf8(output), std::ios::binary | std::ios::trunc);
        out << response->body;
        out.close();
        std::string digest_error;
        if (!utils::HttpClient::verify_file_digest(output, file.expected_hash_algorithm,
                                                   file.expected_hash, digest_error)) {
            throw TransferError("Content verification failed for " + file.name + ": " +
                                digest_error);
        }
        if (progress) {
            progress(response->body.size(), response->body.size());
        }
        return response->body.size();
    }
};

// What the progress contract promises: one total for the whole plan, every file numbered
// against it, and a file named before any of its bytes move. ArtifactTransfer stores the
// callback in a std::function, which copies it, so every call site below passes it through
// std::ref to keep the counters in this object.
struct Progress {
    std::size_t total_across_files = 0;
    std::set<std::size_t> totals_seen;
    std::set<int> file_indexes;
    std::set<std::string> announced;
    int files_in_plan = 0;
    bool operator()(const DownloadProgress& update) {
        total_across_files = update.total_download_size;
        totals_seen.insert(update.total_download_size);
        file_indexes.insert(update.file_index);
        files_in_plan = update.total_files;
        if (update.percent == 0) announced.insert(update.file);
        return true;
    }
};

// A transfer failure has to arrive as a failed check. Left uncaught it aborts the binary,
// which reports no verdict for the check that hit it and none for any check after it.
void check_runs(const std::string& name, const ArtifactTransfer& transfer,
                const DownloadPlan& plan, const std::map<std::string, std::string>& headers,
                DownloadProgressCallback progress) {
    try {
        transfer.execute(plan, headers, progress, {});
        check(name, true);
    } catch (const std::exception& error) {
        check(name, false);
        std::printf("       %s\n", error.what());
    }
}

}  // namespace

int main() {
    const fs::path scratch = lemon::test::make_scratch_dir("repository_fixture");
    utils::set_models_dir((scratch / "models").string());
    const fs::path hub_root = scratch / "hub";
    const std::string repo = "org/fixture-model";

    const std::string gguf_body = std::string("GGUF") + std::string(400, 'x');
    const std::string mmproj_body = std::string("GGUF") + std::string(96, 'm');
    write_file(hub_root / repo / "Model-Q4_K_M.gguf", gguf_body);
    write_file(hub_root / repo / "mmproj-F16.gguf", mmproj_body);
    write_file(hub_root / repo / "config.json", "{\"arch\":\"fixture\"}");

    set_env("LEMONADE_FIXTURE_HUB_ROOT", utils::path_to_utf8(hub_root));
    unset_env("LEMONADE_FIXTURE_HUB_ENDPOINT");
    unset_env("LEMONADE_FIXTURE_HUB_TOKEN");

    const RegistryId host("fixture-hub");

    std::vector<RegistryId> hosts;
    for (const auto& registered : registered_repository_ids()) hosts.push_back(registered);
    check("the fixture host registered itself beside the shipped two",
          std::find(hosts.begin(), hosts.end(), host) != hosts.end());
    check("and claims no id twice",
          std::find(repository_registration_conflicts().begin(),
                    repository_registration_conflicts().end(),
                    host) == repository_registration_conflicts().end());
    const RegistryDescriptor* descriptor = descriptor_for(host);
    check("an alias resolves to it", registry_id_from("fh") != std::nullopt &&
                                        registry_id_from("fh")->str() == host.str());
    bool descriptor_complete = descriptor != nullptr;
    if (descriptor != nullptr) {
        descriptor_complete = !descriptor->display_name.empty() &&
                              !descriptor->scheme_prefix.empty() &&
                              !descriptor->cache_dir_prefix.empty() &&
                              !descriptor->default_revision.empty() &&
                              !descriptor->download_protocol.empty() &&
                              !descriptor->doc.id.empty() && !descriptor->doc.title.empty() &&
                              !descriptor->doc.summary.empty() &&
                              !descriptor->revision_semantics.empty() &&
                              !descriptor->staleness_basis.empty() &&
                              !descriptor->env.empty();
        for (const auto& var : descriptor->env) {
            if (var.name.empty() || var.purpose.empty()) descriptor_complete = false;
        }
    }
    check("its descriptor carries every fact the docs generator reads", descriptor_complete);

    const nlohmann::json sources = model_sources_json(StoreSnapshot{});
    bool in_payload = false;
    if (descriptor != nullptr) {
        for (const auto& row : sources.at("registries")) {
            if (row.value("token", "") != host.str()) continue;
            in_payload = row.value("summary", "") == descriptor->doc.summary &&
                         row.value("scheme", "") == descriptor->scheme_prefix + "://" &&
                         row.value("cache_dir_prefix", "") == descriptor->cache_dir_prefix;
        }
    }
    check("the generated payload already describes it", in_payload);
    const std::string docs_page =
        read_file(fs::path(LEMONADE_REPO_ROOT) / "docs/dev/model-registries.md");
    check("and no page was written for it", !contains(docs_page, "Fixture Hub"));

    // ---- criterion 4: a directory, a plan, no HTTP at all ----
    const auto repository = create_repository(host, side_ref(host, repo, "mmproj-F16.gguf"));
    check("the factory returned it", repository != nullptr);
    if (!repository) return g_failures == 0 ? 0 : 1;
    check("which answers for its host", repository->id().str() == host.str());
    check("and offers no search it did not claim", !repository->supports_search());

    const RepoListing listing = repository->list(repo);
    check("it lists the served files", listing.files.size() == 3);
    check("with a revision of its own", !listing.revision.id.empty() && listing.revision.pinned);

    check("nothing is present before the pull", !repository->present().complete);
    check("and the reason names the repository",
          contains(repository->present().reason, repo));

    const Acquisition acquisition = repository->acquisition();
    check("it pulls through a plan, not a copy loop",
          acquisition.kind == Acquisition::Kind::Plan);
    const DownloadPlan& plan = acquisition.plan;
    check("one file, because a side checkpoint names its own", plan.files.size() == 1);
    check("into the provider-qualified snapshot directory",
          contains(plan.download_path, "fixture-hub--") &&
              contains(plan.download_path, "snapshots/" + listing.revision.id));
    const LocalCopyFileTransfer local_copy;
    const HttpFileTransfer http;
    check("whose URL the local transfer handles", local_copy.handles(plan.files[0].url));
    check("and no socket transfer is even consulted", !http.handles(plan.files[0].url));

    Progress progress;
    check_runs("the shipped transfers take the file:// plan",
               ArtifactTransfer::with_default_transfers(), plan, repository->auth_headers(),
               std::ref(progress));
    const fs::path landed = utils::path_from_utf8(plan.download_path) / "mmproj-F16.gguf";
    check("the bytes are where the layout said", read_file(landed) == mmproj_body);
    check("the progress total covers the whole plan",
          progress.total_across_files == mmproj_body.size());
    check("the served file was read, not moved",
          read_file(hub_root / repo / "mmproj-F16.gguf") == mmproj_body);

    // A file nobody has pulled yet, so a refusal can only come from the digest.
    const auto witness = create_repository(host, side_ref(host, repo, "config.json"));
    DownloadPlan tampered = witness->acquisition().plan;
    tampered.files[0].expected_hash_algorithm = "sha256";
    tampered.files[0].expected_hash = std::string(64, '0');
    bool refused = false;
    try {
        ArtifactTransfer::with_default_transfers().execute(tampered, {}, nullptr, {});
    } catch (const std::exception&) {
        refused = true;
    }
    check("a digest mismatch is a failure, not a retry",
          refused && !fs::exists(utils::path_from_utf8(tampered.download_path) / "config.json"));

    const LocalRef local{repo};
    repository->commit(local, listing.revision);
    check("commit records the revision as active",
          repository->active_revision(local).id == listing.revision.id);
    check("under refs/main beside the snapshot",
          read_file(utils::path_from_utf8(utils::get_hf_cache_dir()) /
                    repository->cache_dir_name(repo) / "refs" / "main") == listing.revision.id);
    check("and the host now reports nothing newer",
          !repository->compare(local, repository->list(repo)).update_available);

    write_file(hub_root / repo / "added-later.gguf", std::string("GGUF") + std::string(8, 'a'));
    const RepoListing newer = repository->list(repo);
    const Staleness stale = repository->compare(local, newer);
    check("a new file on the host is a newer revision", newer.revision.id != listing.revision.id);
    check("reported against the recorded one",
          stale.update_available && stale.local_snapshot_id == listing.revision.id &&
              stale.remote_snapshot_id == newer.revision.id);
    check("naming what changed",
          std::find(stale.changed_files.begin(), stale.changed_files.end(), "added-later.gguf") !=
              stale.changed_files.end());
    const std::string unserved = "org/not-here";
    check("a repository the host does not serve says so",
          throws([&repository, &unserved] { repository->list(unserved); }));
    check("and deletion stays the cache directory",
          repository->deletion().kind == Deletion::Kind::CacheDir);

    // ---- criterion 1: the same host over HTTP, with a credential that is not in the URL ----
    httplib::Server server;
    std::mutex seen_mutex;
    std::vector<std::string> authorizations;
    server.Get(R"(/.*)", [&](const httplib::Request& request, httplib::Response& response) {
        const fs::path served = hub_root / utils::path_from_utf8(request.path.substr(1));
        const std::string body = read_file(served);
        if (body.empty()) {
            response.status = 404;
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(seen_mutex);
            authorizations.push_back(request.get_header_value("Authorization"));
        }
        response.set_content(body, "application/octet-stream");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    std::thread serving([&server] { server.listen_after_bind(); });

    set_env("LEMONADE_FIXTURE_HUB_ENDPOINT", "http://127.0.0.1:" + std::to_string(port));
    set_env("LEMONADE_FIXTURE_HUB_TOKEN", "fixture-secret");

    const auto over_http = create_repository(host, side_ref(host, repo, "Model-Q4_K_M.gguf"));
    const Acquisition fetched = over_http->acquisition();
    check("the same class now answers with HTTP URLs",
          fetched.plan.files.size() == 1 &&
              fetched.plan.files[0].url.rfind("http://127.0.0.1:", 0) == 0);
    check("which the HTTP transfer owns", http.handles(fetched.plan.files[0].url) &&
                                            !local_copy.handles(fetched.plan.files[0].url));
    check("the credential rides with the call",
          over_http->auth_headers().count("Authorization") == 1 &&
              over_http->auth_headers().at("Authorization") == "Bearer fixture-secret");
    check("and never on the plan",
          !contains(fetched.plan.files[0].url, "fixture-secret") &&
              !contains(fetched.plan.download_path, "fixture-secret"));

    // The shipped set is HTTPS-only by policy, so plaintext HTTP is refused before a
    // byte moves. Pinned here because it is the reason the transfer above exists.
    bool plaintext_refused = false;
    try {
        ArtifactTransfer::with_default_transfers().execute(fetched.plan, over_http->auth_headers(),
                                                          nullptr, {});
    } catch (const std::exception&) {
        plaintext_refused = true;
    }
    check("the shipped transfer refuses plaintext HTTP", plaintext_refused);

    // Dispatch takes the first transfer that claims the URL, so the host that owns one
    // narrow scheme has to sit in front of the general one it borrows.
    std::vector<std::unique_ptr<FileTransfer>> schemes;
    schemes.push_back(std::make_unique<LoopbackHttpTransfer>());
    schemes.push_back(std::make_unique<HttpFileTransfer>());
    const ArtifactTransfer over_loopback(std::move(schemes));

    Progress http_progress;
    check_runs("one scheme added beside them takes the plan", over_loopback, fetched.plan,
               over_http->auth_headers(), std::ref(http_progress));
    const fs::path http_landed =
        utils::path_from_utf8(fetched.plan.download_path) / "Model-Q4_K_M.gguf";
    check("and the same plan moves bytes", read_file(http_landed) == gguf_body);
    {
        // Held only to read what the host saw: a request made under this lock deadlocks,
        // because the handler takes it too.
        const std::lock_guard<std::mutex> seen_lock(seen_mutex);
        check("one request reached the host, carrying the token",
              authorizations.size() == 1 && authorizations[0] == "Bearer fixture-secret");
    }

    // ---- one plan, two schemes and two files ----
    // A plan whose only file is also its total cannot tell a plan-wide total from a
    // per-file one, so both halves below stay unpulled and keep different sizes.
    const std::string local_body = std::string("GGUF") + std::string(300, 'l');
    const std::string remote_body = std::string("GGUF") + std::string(120, 'h');
    write_file(hub_root / repo / "mixed-local.gguf", local_body);
    write_file(hub_root / repo / "mixed-http.gguf", remote_body);
    unset_env("LEMONADE_FIXTURE_HUB_ENDPOINT");
    DownloadPlan mixed =
        create_repository(host, side_ref(host, repo, "mixed-local.gguf"))->acquisition().plan;
    set_env("LEMONADE_FIXTURE_HUB_ENDPOINT", "http://127.0.0.1:" + std::to_string(port));
    mixed.files.push_back(
        create_repository(host, side_ref(host, repo, "mixed-http.gguf"))->acquisition().plan
            .files[0]);
    check("one plan carries both schemes",
          mixed.files.size() == 2 && local_copy.handles(mixed.files[0].url) &&
              !local_copy.handles(mixed.files[1].url));

    // One execution, both schemes: the set a plan of one scheme cannot exercise.
    std::vector<std::unique_ptr<FileTransfer>> both_schemes;
    both_schemes.push_back(std::make_unique<LoopbackHttpTransfer>());
    both_schemes.push_back(std::make_unique<LocalCopyFileTransfer>());
    both_schemes.push_back(std::make_unique<HttpFileTransfer>());
    Progress mixed_progress;
    check_runs("and one execution covers both", ArtifactTransfer(std::move(both_schemes)), mixed,
               over_http->auth_headers(), std::ref(mixed_progress));
    check("both files land",
          read_file(utils::path_from_utf8(mixed.download_path) / "mixed-local.gguf") ==
              local_body &&
              read_file(utils::path_from_utf8(mixed.download_path) / "mixed-http.gguf") ==
                  remote_body);
    check("one total for the whole plan, never a per-file one",
          mixed_progress.totals_seen.size() == 1 &&
              mixed_progress.total_across_files == local_body.size() + remote_body.size());
    check("every file numbered against the plan",
          mixed_progress.files_in_plan == 2 &&
              mixed_progress.file_indexes == std::set<int>{1, 2});
    check("each file announced before a byte of it moves",
          mixed_progress.announced.size() == 2);

    server.stop();
    serving.join();
    unset_env("LEMONADE_FIXTURE_HUB_ENDPOINT");
    unset_env("LEMONADE_FIXTURE_HUB_TOKEN");

    // ---- the negative, which is the point of the criterion ----
    int named_in_components = 0;
    for (const std::string& component : {"src/cpp/server/models/store.cpp",
                                         "src/cpp/server/models/sync_service.cpp",
                                         "src/cpp/server/models/ingest.cpp",
                                         "src/cpp/server/model_manager.cpp",
                                         "src/cpp/include/lemon/model_manager.h"}) {
        named_in_components += static_cast<int>(contains(
            read_file(fs::path(LEMONADE_REPO_ROOT) / component), "fixture-hub"));
    }
    check("no component names it", named_in_components == 0);
    int named_under_src = 0;
    for (const auto& entry : fs::recursive_directory_iterator(fs::path(LEMONADE_REPO_ROOT) / "src/cpp")) {
        if (!entry.is_regular_file()) continue;
        if (contains(read_file(entry.path()), "fixture-hub")) ++named_under_src;
    }
    check("nothing under src/cpp names it either", named_under_src == 0);

    if (g_failures > 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll repository-fixture checks passed\n");
    return 0;
}
