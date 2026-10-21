#include "backend_runtime_fixture.h"

#include <type_traits>

using namespace runtime_fixture;

static_assert(!std::is_constructible_v<DefinitionRunner, Json>);
static_assert(!std::is_constructible_v<DefinitionRunner, std::shared_ptr<const CompiledDefinition>>);
static_assert(!std::is_constructible_v<DefinitionRunner, std::shared_ptr<const AuthorizedDefinition>>);
static_assert(!std::is_constructible_v<DefinitionRunner, std::shared_ptr<const ResolvedDefinition>>);
static_assert(!std::is_copy_constructible_v<rt::InvocationGuard>);
static_assert(!std::is_default_constructible_v<rt::InvocationGuard>);
static_assert(!std::is_constructible_v<ConfiguredBackendEntry, std::shared_ptr<const ExecutableDefinition>>);

int main() {
    try {
    check(throws([] { DefinitionRunner runner(nullptr); }, "bound executable"), "RunnerRequiresExecutableDefinition");
    const auto bound = executable();
    const auto entry = ConfiguredBackendEntry::make(bound);
    check(!entry->definition && !entry->ops && entry->v2_ops && entry->executable_definition == bound &&
        !entry->spec.definition && entry->spec.executable_definition == bound, "V2EntryNeverManufacturesV1Definition");
    check(throws([&] { entry->backend_ops()->resolve_checkpoint_path(model(), {}); }, "unavailable") &&
        throws([&] { entry->backend_ops()->is_downloaded(model(), {}); }, "unavailable") &&
        throws([&] { entry->backend_ops()->download_model(model(), false, {}, {}); }, "unavailable"),
        "V2ManagementCannotFallBackToAmbientLegacyEffects");
    const auto& base = ex::Inspection::program(bound->compiled());
    for (int mutation = 0; mutation < 8; ++mutation) {
        auto program = base;
        auto& op = program.operations[0];
        if (mutation == 0) op.kind = "http_bytes";
        if (mutation == 1) op.codec = "raw_bytes.v1";
        if (mutation == 2) op.method = "DELETE";
        if (mutation == 3) op.port = "foreign";
        if (mutation == 4) op.path = "//foreign/path";
        if (mutation == 5) op.timeout_seconds = std::numeric_limits<std::uint64_t>::max();
        if (mutation == 6) op.description.model_types = 0;
        if (mutation == 7) op.description.operation = BackendOperation::Classification;
        const auto compiled = ex::Inspection::test_program(bound->compiled(), program);
        const auto admitted = std::shared_ptr<const AdmittedCatalog>(&bd::Inspection::admitted(*bound), [](const auto*) {});
        const auto rebound = bind_execution_definition(admitted, compiled);
        if (!rebound.definition) throw std::logic_error("Adapter test did not reach final entry validation");
        check(throws([&] { ConfiguredBackendEntry::make(rebound.definition); }, "signature"), "UnsupportedAdapterHasNoEntry:" + std::to_string(mutation));
    }
    Upstream upstream;
    ServerFixture fixture(upstream.port);
    for (const auto operation : {BackendOperation::ChatCompletion, BackendOperation::Completion, BackendOperation::Responses}) for (const bool partial : {false, true}) {
        std::atomic<bool> cancelled{false};
        Json cancelled_result;
        unsigned prior, prior_aborts;
        { std::lock_guard<std::mutex> lock(upstream.mutex); prior = upstream.entered; prior_aborts = upstream.aborted; }
        auto request = std::async(std::launch::async, [&] {
            WrappedServer::RequestCancelScope outer({nullptr, [&] { return cancelled.load(); }});
            WrappedServer::RequestCancelScope nested({});
            return fixture.server->dispatch_completion_operation(operation, {{"model", "cancelled"}, {"hold", true}, {"partial", partial}});
        });
        bool reached;
        {
            std::unique_lock<std::mutex> lock(upstream.mutex);
            reached = upstream.condition.wait_for(lock, std::chrono::seconds(3), [&] { return upstream.entered > prior; });
        }
        check(reached, "V2CancellationReachedActualUpstream");
        const auto peer = fixture.server->chat_completion({{"model", "peer"}, {"marker", 19}});
        const auto cancel_started = std::chrono::steady_clock::now();
        cancelled = true;
        const bool prompt = request.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
        bool upstream_aborted;
        {
            std::unique_lock<std::mutex> lock(upstream.mutex);
            upstream_aborted = upstream.condition.wait_for(lock, std::chrono::seconds(2), [&] { return upstream.aborted > prior_aborts; });
            upstream.release = true; upstream.condition.notify_all();
        }
        cancelled_result = request.get();
        std::printf("V2 cancellation operation=%u partial=%u elapsed_ms=%lld\n", static_cast<unsigned>(operation), static_cast<unsigned>(partial),
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - cancel_started).count()));
        check(prompt && cancelled_result.contains("error") && cancelled_result["error"]["message"] == "Request cancelled by client",
            "V2DisconnectAbortsUpstreamAndPreservesOuterChecker");
        check(upstream_aborted, partial ? "V2UpstreamSeesAbortAfterFirstFragment" : "V2UpstreamSeesAbortBeforeFirstByte");
        check(peer["model"] == "peer" && peer["marker"] == 19 && fixture.server->get_backend_port() == upstream.port &&
            fixture.server->get_state() == ModelState::READY, "ConcurrentV2RequestSurvivesPeerCancellationWithoutReset");
        { std::lock_guard<std::mutex> lock(upstream.mutex); upstream.release = false; }
    }
    const auto before = fixture.launch->events.size();
    std::atomic<bool> cancel_load{true};
    fixture.server->set_load_cancel_flag(&cancel_load);
    auto info = model(); RecipeOptions options;
    check(throws([&] { fixture.server->load("cancelled-load", info, options); }, "cancelled") && fixture.launch->events.size() == before,
        "V2LoadCancellationPerformsNoEffectsOrRetry");
    fixture.server->set_load_cancel_flag(nullptr);
    } catch (const std::exception& error) {
        std::printf("Runtime rejection: %s\n", error.what());
        check(false, "RuntimeBoundariesReachNamedChecks");
    }
    return finish();
}
