#include "backend_catalog_execution_fixture.h"
#include "backend_catalog_publication_internal.h"
#include "backend_typed_hook_registry_internal.h"

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>

using namespace catalog_execution_fixture;

int main() {
    auto complete = build({executable_root(), child("publication-leaf", "catalog-root")});
    check(bool(complete.catalog), "PublicationFixtureFullyFinalized");
    if (!complete.catalog) { dump_errors(complete); return finish(); }
    CatalogPublication publication;
    const auto* descriptor = complete.catalog->registrations().back().descriptor;
    const auto* spec = complete.catalog->registrations().back().spec;
    const auto* ops = complete.catalog->registrations().back().ops;
    std::mutex mutex;
    std::condition_variable condition;
    bool building = false, allow_commit = false;
    auto writer = std::async(std::launch::async, [&] {
        CatalogPublicationGuard guard(publication);
        {
            std::unique_lock<std::mutex> lock(mutex);
            building = true; condition.notify_all();
            condition.wait(lock, [&] { return allow_commit; });
        }
        guard.commit(complete.catalog->data(), complete.catalog);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return building; });
    }
    bool data_blocked = false, server_blocked = false;
    try { publication.data(); } catch (const std::logic_error&) { data_blocked = true; }
    try { publication.server(); } catch (const std::logic_error&) { server_blocked = true; }
    check(data_blocked && server_blocked, "ReadersCannotObservePartiallyBuiltCatalogOrBuiltinFallback");
    {
        std::lock_guard<std::mutex> lock(mutex); allow_commit = true; condition.notify_all();
    }
    try { writer.get(); } catch (const std::exception&) { check(false, "ReadersCannotObservePartiallyBuiltCatalogOrBuiltinFallback"); return finish(); }
    const auto* active = static_cast<const BackendCatalog*>(publication.server());
    const bool matched = active == complete.catalog.get() && publication.data() == active->data().get() &&
        publication.data() && !publication.data()->descriptors.empty() && publication.data()->descriptors.back() == descriptor && active->registrations().back().spec == spec && active->registrations().back().ops == ops;
    check(matched, "OneVisibilityEventPublishesMatchingBorrowedAddresses");
    if (!matched) return finish();
    check(publication.consume_server() == active, "RegistryReadObservesPublishedServerRoot");
    bool replaced = false;
    try { CatalogPublicationGuard replacement(publication); } catch (const std::logic_error&) { replaced = true; }
    check(replaced && publication.server() == active && publication.data() && !publication.data()->descriptors.empty() && publication.data()->descriptors.back() == descriptor, "PublishedGenerationCannotBeReplaced");
    CatalogPublication consumed;
    check(consumed.data() == nullptr, "BuiltinReadConsumesEmptyCatalog");
    replaced = false;
    try { CatalogPublicationGuard replacement(consumed); } catch (const std::logic_error&) { replaced = true; }
    check(replaced && !consumed.server(), "ReplacementAfterConsumptionRejects");
    CatalogPublication registry_consumed;
    check(!registry_consumed.consume_server() && registry_consumed.state() == CatalogPublication::State::BuiltinsConsumed,
        "RegistryReadConsumesBeforeSelectingBuiltinFallback");
    CatalogPublication rollback;
    try { CatalogPublicationGuard guard(rollback); throw std::runtime_error("injected after finalized entries"); }
    catch (const std::runtime_error&) {}
    check(rollback.state() == CatalogPublication::State::Empty && !rollback.server(), "FailedBuildRestoresEmptyWithoutEitherRoot");
    if (rollback.state() != CatalogPublication::State::Empty) return finish();
    if (rollback.state() == CatalogPublication::State::Empty) {
        CatalogPublicationGuard guard(rollback);
        guard.commit(complete.catalog->data(), complete.catalog);
    }
    check(rollback.server() == active, "SuccessfulRetryAfterRollbackPublishesOnce");
    const auto directory = std::filesystem::temp_directory_path() / "publication-global";
    std::filesystem::create_directories(directory);
    const auto definitions = directory / "backends.d";
    std::filesystem::create_directories(definitions);
    auto parent = executable_root();
    auto bad_leaf = child("publication-leaf", "catalog-root");
    bad_leaf["merge"]["install"] = {{"strategy", "github_release"}, {"cases", Json::array({{{"id", "cpu"}, {"backend", {"cpu"}}, {"repo", "fake/engine"}, {"filename", "fake.zip"}}})}};
    std::ofstream(definitions / "a.json") << parent.dump();
    std::ofstream(definitions / "b.json") << bad_leaf.dump();
    bool descendant_rejected = false;
    try { bootstrap_backend_catalog(directory.string()); } catch (const std::exception&) { descendant_rejected = true; }
    check(descendant_rejected && !backend_catalog() && catalog_publication().state() == CatalogPublication::State::Empty,
        "FailingDescendantCannotPublishFinalizedPrefix");
    if (backend_catalog()) return finish();
    std::ofstream(definitions / "b.json") << child("publication-leaf", "catalog-root").dump();
    bootstrap_backend_catalog(directory.string());
    const auto* global = backend_catalog();
    check(global && &all_registrations() == &global->registrations() && &all_descriptors() == &global->data()->descriptors &&
        &frozen_backend_catalog() == global, "GlobalDescriptorAndFactoryReadersUseSamePublication");
    bootstrap_backend_catalog(directory.string());
    check(backend_catalog() == global, "RepeatedIdenticalBootstrapPreservesBorrowedAddresses");
    std::weak_ptr<const BackendCatalog> retained = complete.catalog;
    complete.catalog.reset();
    check(!retained.expired() && publication.server() == active && publication.data()->descriptors.back() == descriptor,
        "PublicationOwnsBorrowedStorageAfterBuilderRelease");
    return finish();
}

void lemon::backends::typed::observe_entry(const char*) { throw std::logic_error("CatalogValidationInvokesNothing"); }
