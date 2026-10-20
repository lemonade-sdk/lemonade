#include "lemon/backends/configured/backend_hook_abi.h"
#include "lemon/backends/configured/backend_hook_registry.h"

#include <cstdio>
#include <functional>

using namespace lemon::backends;
using json = nlohmann::json;
namespace {
int checks = 0, failures = 0, invocations = 0;
void check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", label); }
}
const BackendOps* artifacts() { ++invocations; return nullptr; }
json request(json value) { ++invocations; return value; }
json response(json value, const json&, const std::string&) { ++invocations; return value; }
json launch(const LaunchHookContext&) { ++invocations; return json::object(); }
json operation(const OperationHookContext&, const json&) { ++invocations; return json::object(); }
std::string start_failure(const std::string&, const std::vector<std::string>&) { ++invocations; return ""; }
void sink_operation(const OperationHookContext&, const json&, httplib::DataSink&) { ++invocations; }
}

int main() {
    const auto& installed = installed_backend_hooks();
    audit_hook_abi_metadata(installed);
    auto registrations = installed.registrations();
    const std::vector<HookTarget> targets = {ArtifactHook(&artifacts), RequestHook(&request), ResponseHook(&response),
        LaunchHook(&launch), OperationHook(&operation), StartFailureHook(&start_failure), SinkOperationHook(&sink_operation)};
    for (auto& registration : registrations) registration.target = targets.at(static_cast<std::size_t>(registration.stage));
    audit_hook_abi_metadata(BackendHookRegistry(registrations));
    check(invocations == 0, "ABI audit never invokes executable targets");
    check(registrations.size() == 12, "all installed executable hooks described");
    const auto rejected = [&](const std::function<void(std::vector<HookRegistration>&)>& change) {
        auto copy = registrations;
        change(copy);
        try { audit_hook_abi_metadata(BackendHookRegistry(std::move(copy))); }
        catch (const std::logic_error&) { return true; }
        return false;
    };
    check(rejected([](auto& entries) { entries.pop_back(); }), "missing executable registration rejected");
    check(rejected([](auto& entries) { entries[0].id = "unknown.executable"; }), "unknown executable registration rejected");
    check(rejected([&](auto& entries) { entries[0].stage = HookStage::Request; entries[0].target = targets[1]; }), "executable stage change rejected");
    check(invocations == 0, "rejected audits never invoke executable targets");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
