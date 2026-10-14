// Pins the artifact-source seam: what a checkpoint is (ArtifactRef), what a
// source answers (ArtifactSource), and the registration that carries a host's
// descriptor without a second table of facts.

#include "lemon/models/artifact_ref.h"
#include "lemon/models/artifact_source.h"
#include "lemon/models/checkpoint_grammar.h"
#include "lemon/models/fetchable_repository.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <utility>
#include <string>
#include <vector>

using lemon::models::ArtifactKind;
using lemon::models::ArtifactRef;
using lemon::models::Acquisition;
using lemon::models::Deletion;
using lemon::models::FetchableRepository;
using lemon::models::LocalRef;
using lemon::models::Presence;
using lemon::models::RegistryDescriptor;
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

static RegistryDescriptor fake_descriptor() {
    return RegistryDescriptor{RegistryId("zzfake"),
                              {"zzfake", "zf"},
                              "Fake Host",
                              "zf",
                              "zzfake--",
                              "trunk",
                              ""};
}

// The whole interface has to be implementable from the value types alone, or the
// seam leaks something the host cannot supply.
class FakeRepository : public FetchableRepository {
  public:
    explicit FakeRepository(ArtifactRef ref) : FetchableRepository(std::move(ref)) {}

    ArtifactKind kind() const override { return ArtifactKind::Registry; }
    std::string resolve() const override { return ""; }
    Presence present() const override { return Presence{}; }
    Acquisition acquisition() const override { return Acquisition{}; }
    Deletion deletion() const override { return Deletion{}; }

    RegistryId id() const override { return RegistryId("zzfake"); }
    Revision default_revision() const override { return Revision{"trunk", false}; }

    RepoListing list(const std::string& repo_id, const std::string& revision) const override {
        return RepoListing{repo_id, Revision{revision, false}, {}};
    }
    std::string file_url(const RepoRef& ref) const override {
        return "zf://" + ref.repo_id + "/" + ref.path;
    }
    bool can_go_stale() const override { return true; }
    Revision active_revision(const LocalRef&) const override { return Revision{"trunk", false}; }
    void set_active_revision(const LocalRef&, const Revision&) override {}
    Staleness compare(const LocalRef&, const RepoListing& listing) const override {
        Staleness staleness;
        staleness.remote_snapshot_id = listing.revision.id;
        return staleness;
    }
    void commit(const LocalRef&, const Revision&) override {}
    std::string cache_dir_name(const std::string& repo_id) const override {
        return "zzfake--" + repo_id;
    }
};

LEMONADE_REGISTER_ARTIFACT_REPOSITORY(FakeRepository, fake_descriptor());

// A second host claiming the same id: reported, not silently decided by link order.
class DuplicateRepository : public FakeRepository {
  public:
    using FakeRepository::FakeRepository;

    RegistryId id() const override { return RegistryId("zzfake"); }
};

LEMONADE_REGISTER_ARTIFACT_REPOSITORY(DuplicateRepository, fake_descriptor());

static void test_two_refs_are_the_same_checkpoint_only_when_their_provenance_is() {
    ArtifactRef ref;
    ref.registry = RegistryId("huggingface");
    ref.repo_id = "someone/somewhere";
    ref.variant = "fp16";

    check("a checkpoint is the same ref when nothing moved", ref == ArtifactRef(ref));

    ArtifactRef other_variant = ref;
    other_variant.variant = "q4";
    check("a different variant is a different checkpoint", !(ref == other_variant));

    ArtifactRef optional = ref;
    optional.required = false;
    check("a checkpoint that need not be fetched is not the same ref", !(ref == optional));
}

static void test_registration_builds_the_host_it_names() {
    const RegistryId fake = RegistryId("zzfake");

    const std::unique_ptr<FetchableRepository> repository =
        lemon::models::create_repository(fake, ArtifactRef{});
    check("a registered host builds", repository != nullptr);
    if (repository != nullptr) {
        check("and reports its own id", repository->id() == fake);
        check("its default revision is not inherited from another host",
              repository->default_revision().id == "trunk");
        check("it answers for its own cache namespace",
              repository->cache_dir_name("org/repo") == "zzfake--org/repo");
        check("a source knows what kind it is",
              repository->kind() == ArtifactKind::Registry);
    }

    check("an unregistered host builds nothing",
          lemon::models::create_repository(RegistryId("zznobody"), ArtifactRef{}) == nullptr);
}

static void test_registration_carries_the_descriptor() {
    const RegistryDescriptor* descriptor = lemon::models::descriptor_for(RegistryId("zzfake"));
    check("a registered host has a descriptor row", descriptor != nullptr);
    if (descriptor != nullptr) {
        check("the row came from the registration, not a second table",
              descriptor->cache_dir_prefix == "zzfake--" &&
              descriptor->default_revision == "trunk");
        check("looking up by id does not resolve an alias",
              lemon::models::descriptor_for(RegistryId("zf")) == nullptr);
    }

    const std::vector<RegistryId> registered = lemon::models::registered_repository_ids();
    check("the registered list includes the new host",
          std::find(registered.begin(), registered.end(), RegistryId("zzfake")) !=
              registered.end());
}

static void test_a_host_claimed_twice_is_reported() {
    const std::vector<RegistryId> conflicts = lemon::models::repository_registration_conflicts();
    check("the duplicate claim is reported rather than won silently",
          std::find(conflicts.begin(), conflicts.end(), RegistryId("zzfake")) !=
              conflicts.end());
}

static void test_the_builtin_hosts_are_untouched() {
    // A test registering a host must not disturb the rows production reads.
    const RegistryDescriptor* huggingface =
        lemon::models::descriptor_for(RegistryId("huggingface"));
    check("the default host still resolves", huggingface != nullptr);
    if (huggingface != nullptr) {
        check("with its own namespace", huggingface->cache_dir_prefix == "models--");
    }
}


// The built-in hosts are reached the same way as any registered one, so these
// assert the port's answers without a network.
static void test_the_built_in_hosts_answer_through_the_port() {
    const std::unique_ptr<FetchableRepository> huggingface =
        lemon::models::create_repository(RegistryId("huggingface"), ArtifactRef{});
    const std::unique_ptr<FetchableRepository> modelscope =
        lemon::models::create_repository(RegistryId("modelscope"), ArtifactRef{});

    check("both hosts build from registration alone",
          huggingface != nullptr && modelscope != nullptr);
    if (huggingface == nullptr || modelscope == nullptr) {
        return;
    }

    check("each reports the host it was built for",
          huggingface->id().str() == "huggingface" && modelscope->id().str() == "modelscope");
    check("each keeps its own cache namespace",
          huggingface->cache_dir_name("org/repo") == "models--org--repo" &&
          modelscope->cache_dir_name("org/repo") == "modelscope--models--org--repo");
    check("each keeps its own default revision",
          huggingface->default_revision().id == "main" &&
          modelscope->default_revision().id == "master");
    check("a branch default is not pinned", !huggingface->default_revision().pinned);
    check("both can go stale", huggingface->can_go_stale() && modelscope->can_go_stale());
    check("deleting a registry model removes its cache directory",
          huggingface->deletion().kind == lemon::models::Deletion::Kind::CacheDir);
}

static void test_the_host_is_stamped_into_the_source() {
    ArtifactRef without_host;
    without_host.repo_id = "org/repo";
    const std::unique_ptr<FetchableRepository> repository =
        lemon::models::create_repository(RegistryId("modelscope"), without_host);
    check("a caller that names no host still gets one",
          repository != nullptr && repository->id().str() == "modelscope");
}

static void test_the_recorded_revision_round_trips() {
    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path() /
                          ("registry_repo_" + std::to_string(
                                                 std::chrono::steady_clock::now()
                                                     .time_since_epoch()
                                                     .count()));
    fs::create_directories(temp);
    // The repository resolves a local ref against the models dir, not the cache
    // dir: get_hf_cache_dir() is what it asks.
    lemon::utils::set_models_dir(lemon::utils::path_to_utf8(temp));

    const std::unique_ptr<FetchableRepository> repository =
        lemon::models::create_repository(RegistryId("huggingface"), ArtifactRef{});
    if (repository == nullptr) {
        check("the port builds a source to record a revision on", false);
        return;
    }
    const LocalRef local{"org/repo"};

    check("nothing is recorded before a download",
          repository->active_revision(local).id.empty());
    check("and a source says why it is not present",
          !repository->present().complete && !repository->present().reason.empty());

    const std::string commit(40, 'a');
    repository->set_active_revision(local, Revision{commit, false});
    const Revision recorded = repository->active_revision(local);
    check("the revision a download committed is read back", recorded.id == commit);
    check("a full commit SHA is pinned", recorded.pinned);

    const lemon::models::Staleness newer =
        repository->compare(local, RepoListing{"org/repo", Revision{"b", false}, {}});
    check("a different remote snapshot is an update", newer.update_available);
    check("both snapshot ids are reported",
          newer.local_snapshot_id == commit && newer.remote_snapshot_id == "b");
    check("the same snapshot is not an update",
          !repository->compare(local, RepoListing{"org/repo", Revision{commit, true}, {}})
               .update_available);

    // Never-downloaded, not overwritten: an empty revision writes nothing, which
    // is how a partial download keeps its recorded ref.
    check("an unrecorded model reports no update rather than a false one",
          !repository->compare(LocalRef{"org/never-downloaded"},
                               RepoListing{"org/never-downloaded", Revision{"c", false}, {}})
               .update_available);

    fs::remove_all(temp);
}

int main() {
    test_two_refs_are_the_same_checkpoint_only_when_their_provenance_is();
    test_registration_builds_the_host_it_names();
    test_registration_carries_the_descriptor();
    test_a_host_claimed_twice_is_reported();
    test_the_builtin_hosts_are_untouched();
    test_the_built_in_hosts_answer_through_the_port();
    test_the_host_is_stamped_into_the_source();
    test_the_recorded_revision_round_trips();

    if (g_failures == 0) {
        std::printf("All artifact source tests passed.\n");
        return 0;
    }
    std::printf("%d artifact source test(s) failed.\n", g_failures);
    return 1;
}
