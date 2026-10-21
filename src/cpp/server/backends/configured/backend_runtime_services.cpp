#include "backend_runtime_services_internal.h"
#include "lemon/backends/configured/backend_definition_binding.h"

#include <algorithm>
#include <atomic>
#include <limits>

namespace lemon::backends::runtime {
namespace {
std::atomic<std::uint64_t> next_nonce{1};
std::uint64_t invocation_nonce() {
    auto nonce = next_nonce.load();
    do {
        if (nonce == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("Invocation nonce exhausted");
    } while (!next_nonce.compare_exchange_weak(nonce, nonce + 1));
    return nonce;
}
}

Frame::Frame(const ExecutableDefinition& owner) : executable(&owner), nonce(invocation_nonce()) {}
void check_cancelled(const utils::RequestCancelToken& cancel) {
    if (cancel.cancelled()) throw utils::HttpClientCancellationException(42, "Request cancelled by client");
}
InvocationGuard::InvocationGuard(Frame& frame, const ExecutableDefinition& executable, std::size_t slot,
    const execution::SymbolicHook& hook, SlotOwnerKind kind, const std::string& owner,
    const std::string& stage, utils::RequestCancelToken cancel)
    : frame_(frame), executable_(&executable), slot_(slot), owner_(hook.slot), nonce_(frame.nonce),
      effects_(hook.effects), thread_(std::this_thread::get_id()), cancel_(std::move(cancel)) {
    check_cancelled(cancel_);
    if (frame_.executable != executable_ || frame_.nonce != nonce_) throw std::logic_error("Foreign invocation frame");
    if (owner_.owner_kind != kind || owner_.owner_id != owner || owner_.stage != stage)
        throw std::logic_error("Bound slot owner/stage mismatch");
    if (frame_.active_slot) throw std::logic_error("Nested bound slot invocation");
    for (const auto& input : hook.inputs) {
        if (input.producer ? !frame_.produced.count(*input.producer) : !frame_.facts.count(input.fact))
            throw std::logic_error("Missing bound slot fact");
    }
    for (const auto dependency : hook.dependencies) if (!frame_.produced.count(dependency))
        throw std::logic_error("Missing bound slot dependency");
    frame_.active_slot = slot_;
}
InvocationGuard::~InvocationGuard() { active_ = false; frame_.active_slot.reset(); }
void InvocationGuard::check_active() const {
    if (!active_ || thread_ != std::this_thread::get_id() || frame_.executable != executable_ || frame_.nonce != nonce_ || frame_.active_slot != slot_)
        throw std::logic_error("Inactive bound slot invocation");
    check_cancelled(cancel_);
}
void InvocationGuard::check(typed::Effect effect, std::size_t allowance) {
    check_active();
    if (std::find(effects_.begin(), effects_.end(), effect) == effects_.end())
        throw std::logic_error("Effect outside bound slot");
    if (allowance > remaining_) throw std::length_error("Invocation service allowance exceeded");
    remaining_ -= allowance;
}
SelectionServices::SelectionServices(InvocationGuard& guard, std::vector<std::string> supported)
    : guard_(guard), supported_(std::move(supported)) {}
bool SelectionServices::supports(const std::string& backend) {
    guard_.check(typed::Effect::SupportQuery);
    return std::find(supported_.begin(), supported_.end(), backend) != supported_.end();
}
typed::DeviceCheck SelectionServices::validate_device(const typed::DeviceValidationInput& input) {
    guard_.check(typed::Effect::DeviceQuery);
    auto backend = input.backend;
    if (backend.rfind("rocm-", 0) == 0) backend = "rocm";
    if (std::find(supported_.begin(), supported_.end(), backend) == supported_.end())
        throw std::logic_error("Device query outside selected backend scope");
    try { BackendUtils::validate_device_backend_match(backend, input.device); return {true, ""}; }
    catch (const std::invalid_argument& error) { return {false, error.what()}; }
}

}
