// Pins the artifact-source seam: what a checkpoint is (ArtifactRef), what a
// source answers (ArtifactSource), and the registration that carries a host's
// descriptor without a second table of facts.

#include "lemon/models/artifact_ref.h"
#include "lemon/models/artifact_source.h"
#include "lemon/models/fetchable_repository.h"
#include "lemon/models/locator.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using lemon::models::ArtifactKind;
using lemon::models::ArtifactRef;
using lemon::models::Acquisition;
using lemon::models::Deletion;
using lemon::models::FetchableRepository;
using lemon::models::LocalRef;
using lemon::models::Locator;
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
    FakeRepository() : FetchableRepository(ArtifactRef{}) {}

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
    RegistryId id() const override { return RegistryId("zzfake"); }
};

LEMONADE_REGISTER_ARTIFACT_REPOSITORY(DuplicateRepository, fake_descriptor());

static void test_a_ref_names_the_checkpoint_a_locator_points_at() {
    const Locator registry_locator = Locator::from_checkpoint("someone/somewhere:fp16", "hf");
    const ArtifactRef ref = ArtifactRef::from_locator(registry_locator, true);

    check("a registry ref keeps its host", ref.registry.has_value() &&
          ref.registry->str() == "huggingface");
    check("a ref keeps the repository", ref.repo_id == "someone/somewhere");
    check("a ref keeps the variant", ref.variant == "fp16");
    check("a required checkpoint says so", ref.required);

    ArtifactRef optional_ref = ArtifactRef::from_locator(registry_locator, false);
    check("required distinguishes two otherwise equal refs", !(optional_ref == ref));

    // An absent source means the default host, so a file the user placed names
    // its origin rather than relying on the path's shape.
    const Locator file_locator =
        Locator::from_checkpoint("/tmp/somewhere/thing.gguf", "local_path");
    const ArtifactRef file_ref = ArtifactRef::from_locator(file_locator, true);
    check("bytes that never came from a host name no host", !file_ref.registry.has_value());
    check("and are named by where they were found",
          file_ref.repo_id == "/tmp/somewhere/thing.gguf");
}

static void test_registration_builds_the_host_it_names() {
    const RegistryId fake = RegistryId("zzfake");

    const std::unique_ptr<FetchableRepository> repository =
        lemon::models::create_repository(fake);
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
          lemon::models::create_repository(RegistryId("zznobody")) == nullptr);
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

int main() {
    test_a_ref_names_the_checkpoint_a_locator_points_at();
    test_registration_builds_the_host_it_names();
    test_registration_carries_the_descriptor();
    test_a_host_claimed_twice_is_reported();
    test_the_builtin_hosts_are_untouched();

    if (g_failures == 0) {
        std::printf("All artifact source tests passed.\n");
        return 0;
    }
    std::printf("%d artifact source test(s) failed.\n", g_failures);
    return 1;
}
