#pragma once

#include "backend_definition_execution_internal.h"
#include "backend_hook_types.h"
#include "lemon/backends/configured/launch_planner.h"
#include "lemon/utils/http_client.h"

#include <set>
#include <thread>

namespace lemon::backends {

class DefinitionRunner;
class ExecutableDefinition;

namespace runtime {

struct Frame {
    execution::InvocationFrame data;
    std::set<typed::Fact> facts;
    std::set<std::size_t> produced;
    std::set<std::string> explicit_options;
    const ExecutableDefinition* executable;
    const std::uint64_t nonce;
    std::optional<std::size_t> active_slot;
    explicit Frame(const ExecutableDefinition& owner);
    Frame(Frame&&) = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

class InvocationGuard final {
public:
    ~InvocationGuard();
    InvocationGuard(const InvocationGuard&) = delete;
    InvocationGuard& operator=(const InvocationGuard&) = delete;
    void check(typed::Effect effect, std::size_t allowance = 1);
    void check_active() const;
private:
    InvocationGuard(Frame& frame, const ExecutableDefinition& executable, std::size_t slot,
        const execution::SymbolicHook& hook, SlotOwnerKind kind, const std::string& owner,
        const std::string& stage, utils::RequestCancelToken cancel);
    Frame& frame_;
    const ExecutableDefinition* executable_;
    const std::size_t slot_;
    const LogicalHookSlot owner_;
    const std::uint64_t nonce_;
    const std::vector<typed::Effect> effects_;
    const std::thread::id thread_;
    const utils::RequestCancelToken cancel_;
    std::size_t remaining_ = 1024 * 1024;
    bool active_ = true;
    friend class ::lemon::backends::DefinitionRunner;
#ifdef LEMONADE_RUNTIME_TEST
    friend struct TestAccess;
#endif
};

void check_cancelled(const utils::RequestCancelToken& cancel);

class SelectionServices final : public typed::BackendSelectionServices {
public:
    SelectionServices(InvocationGuard& guard, std::vector<std::string> supported);
    bool supports(const std::string& backend) override;
    typed::DeviceCheck validate_device(const typed::DeviceValidationInput& input) override;
private:
    InvocationGuard& guard_;
    const std::vector<std::string> supported_;
};

}
}
