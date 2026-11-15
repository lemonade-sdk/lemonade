#include "lemon/server/hybrid_coder.h"
#include "lemon/server/hybrid_context_policy.h"
#include "lemon/server/hybrid_history.h"
#include "lemon/server/hybrid_usage.h"
#include "lemon/server/hybrid_worker_policy.h"
#include "lemon/server/hybrid_slot_scope.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <set>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_manager.h"
#include "lemon/router.h"
#include "lemon/server/server_context.h"
#include "lemon/streaming_proxy.h"

namespace lemon {
namespace {

using json = nlohmann::json;

constexpr int kOrchestratorSlot = 0;
constexpr int kWorkerSlot = 1;
constexpr size_t kMaxSavedSessions = 8;
constexpr size_t kMaxExternalHistoryBytes = 32 * 1024 * 1024;
constexpr int kMaxInternalCalls = 20;
constexpr size_t kMaxTaskBytes = 8192;
constexpr size_t kMaxSseLineBytes = 8 * 1024 * 1024;
constexpr size_t kMaxToolArgumentsBytes = 4 * 1024 * 1024;
constexpr int kRoutingMaxTokens = 2048;

class HybridOutputIncomplete : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RoutingIncomplete : public std::runtime_error {
public:
    explicit RoutingIncomplete(const std::string& message, bool limit = false)
        : std::runtime_error(message), output_limit(limit) {}

    bool output_limit;
};

const char* kDelegateName = "hybrid_delegate_task";
const char* kRespondName = "hybrid_respond";
const char* kWorkerDoneName = "hybrid_worker_done";
const char* kWorkerToolsName = "hybrid_worker_use_tools";
const char* kOrchestratorPrompt =
    "You are the long-lived coding orchestrator. Maintain the user's overall goals, "
    "architecture and constraints. Delegate concrete investigation, implementation and "
    "test tasks using hybrid_delegate_task. Give each task the minimum necessary context, "
    "including file paths, acceptance criteria, user requirements and relevant client "
    "system/developer instructions (especially tool-use and project rules). Prefer the "
    "smallest change that satisfies the behavior and covers relevant edge cases. "
    "After a worker reports a correct change and adequate verification (or a concrete "
    "environment blocker), finish unless an acceptance criterion remains unresolved. "
    "Do not re-delegate merely to repair missing test dependencies or recreate an "
    "environment that is outside the user's coding request. "
    "Never assume "
    "a worker succeeded without reading its result. Preserve decisions in your own "
    "conversation, but avoid copying worker logs or tool transcripts into it. You can "
    "use the client's other tools yourself when justified. Do not mention internal "
    "delegation unless helpful to the user.";
const char* kWorkerPrompt =
    "You are a short-lived coding worker. Execute only the assigned task using the "
    "client-provided tools where needed. Do not delegate further. If the task is "
    "blocked, say so rather than claiming success. When done, return a concise, "
    "accurate result: changes made, tests actually run, failures and any useful "
    "file references. Do not paste large file contents or tool logs into the result. "
    "Prefer the smallest correct local change; test representative counterexamples "
    "before introducing a new parser, helper or unrelated release note. "
    "After implementing a targeted fix, attempt relevant verification. Fix actual "
    "code failures, but distinguish them from missing dependencies, inaccessible "
    "services, or broken test environments. If verification is blocked by such "
    "an environmental problem, report the exact limitation and finish; do not "
    "repeatedly retry the same blocker, fabricate dependency stubs, or build a "
    "replacement test environment. An unavailable dependency or incompatible "
    "interpreter is a verification limitation, not a reason to repair the "
    "development environment unless the user requested that work. A targeted "
    "check that exercises changed behavior is useful; unrelated environment "
    "troubleshooting is not. Preserve the user's constraints. "
    "When implementation and appropriate verification are finished or honestly "
    "blocked, finish with either hybrid_worker_done or a concise final text "
    "report. Both are private handoffs to the orchestrator. Only request "
    "another client tool if its result can resolve an outstanding coding or "
    "verification question. Do not repeat already established observations.";

struct Session {
    std::string backend_identity;
    json visible = json::array();
    json orchestrator = json::array();
    json worker = json::array();
    json delegation = nullptr;
    std::set<std::string> pending_tool_ids;
    std::set<std::string> used_tool_ids;
    json queued_user_messages = json::array();
    std::string last_tool_round;
    unsigned repeated_tool_rounds = 0;
    int pending_slot = -1;
    int delegations = 0;
    int64_t billed_prompt = 0;
    int64_t billed_output = 0;
    int64_t orchestrator_prompt = 0;
    int64_t worker_prompt = 0;
    bool worker_active = false;
    hybrid_usage::Totals perf;
    uint64_t worker_resets = 0;
    uint64_t worker_rollovers = 0;
    uint64_t orchestrator_rollovers = 0;
    uint64_t checkpoint_calls = 0;
    std::array<hybrid_context_policy::Observation, 2> role_observations{};
};

// Model-local logical conversations, physical KV leases and Hybrid lock.
// A long running Hybrid inference on one model must not reject another model.
struct SessionStore {
    std::mutex mutex;
    std::map<std::string, Session> sessions;
    std::map<std::string, std::string> active_sessions;
    std::map<std::string, uint64_t> observed_ordinary_generation;
};
std::mutex g_store_registry_mutex;
std::map<std::string, std::shared_ptr<SessionStore>> g_session_stores;

std::shared_ptr<SessionStore> session_store_for(const std::string& model) {
    std::lock_guard<std::mutex> lock(g_store_registry_mutex);
    auto& store = g_session_stores[model];
    if (!store) store = std::make_shared<SessionStore>();
    return store;
}

json error_response(const std::string& message, const std::string& code) {
    return {{"error", {{"message", message}, {"type", "invalid_request_error"},
                        {"code", code}}}};
}

std::string error_message(const json& response) {
    if (response.contains("error") && response["error"].is_object()) {
        return response["error"].value("message", "Backend error");
    }
    return "Backend error";
}

// Read-only KV diagnostics. Never log user prompts or tool arguments. This is
// disabled for normal inference to avoid extra backend HTTP calls and latency.
bool trace_slot_usage() {
    const char* flag = std::getenv("LEMONADE_HYBRID_PROFILE");
    return flag && std::string(flag) == "1";
}

void log_slots(const std::string& model, const std::string& phase, const json& slots) {
    if (!trace_slot_usage() || !slots.is_array()) return;
    json concise = json::array();
    for (const auto& slot : slots) {
        if (!slot.is_object()) continue;
        json item = json::object();
        for (const char* key : {"id", "n_ctx", "n_past", "n_decoded",
                                "n_prompt_tokens_processed", "n_prompt_tokens_cache",
                                "is_processing"}) {
            if (slot.contains(key)) item[key] = slot[key];
        }
        concise.push_back(std::move(item));
    }
    LOG(INFO, "HybridProfile") << "model=" << model << " phase=" << phase
        << " slots=" << concise.dump() << std::endl;
}

void snapshot_slots(ServerContext& ctx, const std::string& model,
                    const std::string& phase) {
    if (!trace_slot_usage()) return;
    try {
        log_slots(model, phase, ctx.router->get_slots(model));
    } catch (const std::exception& e) {
        LOG(WARNING, "HybridProfile") << "Could not read slots: " << e.what() << std::endl;
    }
}

// Lightweight measurements from existing backend responses. Unlike
// LEMONADE_HYBRID_PROFILE this never queries /slots and is safe for timed runs.
bool trace_inference_metrics() {
    const char* flag = std::getenv("LEMONADE_HYBRID_METRICS");
    return flag && std::string(flag) == "1";
}

double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

using hybrid_history::normalized_message;
using hybrid_history::canonical_history_message;
using hybrid_history::history_messages_equivalent;
using hybrid_history::history_mismatch_field;

json external_assistant(const json& message) {
    json result = normalized_message(message);
    if (result.value("role", "") != "assistant") {
        throw std::runtime_error("Backend did not return an assistant message");
    }
    return result;
}

std::string backend_identity(ServerContext& ctx, const std::string& model) {
    const RecipeOptions options = ctx.router->get_model_recipe_options(model);
    if (options.get_recipe() != "llamacpp") {
        throw std::runtime_error("Hybrid model '" + model +
                                 "' is not a loaded llama.cpp model");
    }

    // ModelRoute already resolved aliases, but use the same public name as
    // get_all_loaded_models() instead of assuming the caller used that spelling.
    const std::string loaded_name = ctx.model_manager->get_public_model_name(
        ctx.router->canonical_model_name(model));
    const json loaded = ctx.router->get_all_loaded_models();
    const json* selected = nullptr;
    if (loaded.is_array()) {
        for (const auto& entry : loaded) {
            if (!entry.is_object() || entry.value("model_name", "") != loaded_name) continue;
            if (selected) {
                throw std::runtime_error("Ambiguous loaded model for Hybrid-Coder-Mode: " + loaded_name);
            }
            selected = &entry;
        }
    }
    if (!selected || selected->value("recipe", "") != "llamacpp" ||
        selected->value("type", "") != "llm") {
        throw std::runtime_error("Hybrid model '" + loaded_name +
                                 "' must be a loaded llama.cpp LLM");
    }
    return loaded_name + ":" + selected->value("backend_url", "") +
           ":" + selected->value("checkpoint", "") +
           ":" + std::to_string(selected->value("pid", 0));
}

struct SlotPressure {
    int64_t context = 0;
    int64_t occupied = 0;
};

std::array<SlotPressure, 2> check_slots(ServerContext& ctx, const std::string& model) {
    const json slots = ctx.router->get_slots(model);
    if (!slots.is_array() || slots.size() != 2 || !slots[0].is_object() ||
        !slots[1].is_object() || slots[0].value("id", -1) != kOrchestratorSlot ||
        slots[1].value("id", -1) != kWorkerSlot ||
        slots[0].value("n_ctx", 0) < 512 || slots[1].value("n_ctx", 0) < 512 ||
        std::abs(slots[0].value("n_ctx", 0) - slots[1].value("n_ctx", 0)) > 512) {
        throw std::runtime_error("Hybrid-Coder-Mode requires two llama.cpp slots for '" +
                                 model + "' with usable per-slot contexts");
    }
    log_slots(model, "request_begin", slots);
    return {SlotPressure{slots[0].value("n_ctx", int64_t{0}),
                         slots[0].value("n_past", int64_t{0})},
            SlotPressure{slots[1].value("n_ctx", int64_t{0}),
                         slots[1].value("n_past", int64_t{0})}};
}

void erase_slot(ServerContext& ctx, const std::string& model, int slot_id) {
    hybrid_slot_scope::InferenceGuard model_gate(model);
    const json result = ctx.router->slots_action(slot_id, "erase", json::object(), model);
    if (!result.is_object() || result.contains("error") ||
        result.value("id_slot", -1) != slot_id) {
        throw std::runtime_error("Could not erase hybrid llama.cpp slot " +
                                 std::to_string(slot_id) + " for '" + model + "'");
    }
    snapshot_slots(ctx, model, "erase_slot_" + std::to_string(slot_id));
    if (trace_inference_metrics()) {
        LOG(INFO, "HybridMetrics") << "event=slot_erase model=" << model
            << " slot=" << slot_id << std::endl;
    }
}

json delegate_tool() {
    return {{"type", "function"},
            {"function", {{"name", kDelegateName},
                           {"description", "Delegate one bounded coding task to an isolated, resettable worker."},
                           {"parameters", {{"type", "object"},
                                           {"properties", {{"task", {{"type", "string"},
                                                                        {"description", "Self-contained task with paths, constraints and acceptance criteria."}}}}},
                                           {"required", json::array({"task"})},
                                           {"additionalProperties", false}}}}}};
}

json tool_list(const json& request, bool include_delegate) {
    json tools = request.value("tools", json::array());
    if (!tools.is_array()) {
        throw std::runtime_error("Hybrid tool declarations must be an array");
    }
    for (const auto& tool : tools) {
        const std::string name = tool.value("function", json::object()).value("name", "");
        if (name == kDelegateName || name == kRespondName ||
            name == kWorkerDoneName || name == kWorkerToolsName) {
            throw std::runtime_error("Internal hybrid tool names are reserved by Lemonade");
        }
    }
    if (include_delegate) tools.push_back(delegate_tool());
    return tools;
}

std::string instruction_text(const json& message) {
    if (!message.contains("content") || message["content"].is_null()) return "";
    const json& content = message["content"];
    if (content.is_string()) return content.get<std::string>();
    if (!content.is_array()) {
        throw std::runtime_error("Hybrid system/developer instructions must contain text");
    }
    std::string text;
    for (const auto& part : content) {
        if (!part.is_object() || part.value("type", "") != "text" ||
            !part.contains("text") || !part["text"].is_string()) {
            throw std::runtime_error("Hybrid system/developer instructions may contain only text parts");
        }
        if (!text.empty()) text += "\n";
        text += part["text"].get<std::string>();
    }
    return text;
}

void initialize(Session& state, ServerContext& ctx, const json& incoming,
                const std::string& model) {
    if (incoming.empty() || incoming.back().value("role", "") != "user") {
        throw std::runtime_error("Start Hybrid-Coder-Mode with a fresh conversation ending in a user message");
    }
    std::string system_prompt = kOrchestratorPrompt;
    json dialogue = json::array();
    bool dialogue_started = false;
    for (const auto& message : incoming) {
        const std::string role = message.value("role", "");
        if (role == "system" || role == "developer") {
            if (dialogue_started) {
                throw std::runtime_error("Initial system/developer instructions must precede user messages in Hybrid-Coder-Mode");
            }
            const std::string text = instruction_text(message);
            if (!text.empty()) {
                system_prompt += "\n\nClient " + role + " instructions:\n" + text;
            }
        } else if (role == "user") {
            dialogue_started = true;
            dialogue.push_back(message);
        } else {
            throw std::runtime_error("Start Hybrid-Coder-Mode in a new chat, not an existing tool or assistant history");
        }
    }
    erase_slot(ctx, model, kOrchestratorSlot);
    erase_slot(ctx, model, kWorkerSlot);
    state.visible = incoming;
    // A number of llama.cpp chat templates allow only one leading system message.
    // Preserve the client's instruction order without placing any system/developer
    // role after the Hybrid-Coder orchestrator preamble.
    state.orchestrator = json::array({{{"role", "system"}, {"content", system_prompt}}});
    for (const auto& message : dialogue) state.orchestrator.push_back(message);
}

void ingest(Session& state, const json& incoming, bool may_rebase) {
    size_t append_from = state.visible.size();
    bool intact = incoming.size() >= append_from;
    if (intact) {
        for (size_t i = 0; i < append_from; ++i) {
            if (!hybrid_history::same_or_pruned(state.visible[i], incoming[i], true)) {
                intact = false;
                break;
            }
        }
    }
    if (!intact) {
        if (!may_rebase) {
            throw std::runtime_error("Hybrid history changed; supply a stable hybrid session "
                                     "ID for verified client compaction");
        }
        const auto rebased = hybrid_history::retained_suffix_end(state.visible, incoming);
        if (!rebased) {
            throw std::runtime_error("Hybrid history has no unique retained suffix; "
                                     "refusing unsafe compaction or rewind");
        }
        append_from = *rebased;
        // Keep authoritative Orchestrator/Worker histories untouched.
        state.visible = json::array();
        for (size_t i = 0; i < append_from; ++i) {
            state.visible.push_back(incoming[i]);
        }
    }
    if (incoming.size() == append_from) {
        throw std::runtime_error("Hybrid conversation has no new messages");
    }
    json received_tools = json::array();
    const int pending_before = state.pending_slot;
    for (size_t i = append_from; i < incoming.size(); ++i) {
        const json& message = incoming[i];
        const std::string role = message.value("role", "");
        if (role == "tool") {
            const std::string call_id = message.value("tool_call_id", "");
            if (state.pending_slot < 0 || !state.pending_tool_ids.erase(call_id)) {
                throw std::runtime_error("Unexpected or duplicate tool_call_id: " + call_id);
            }
            // Do not collect arbitrarily large output bodies in a second
            // copy. The signature is unavailable for >64K of tool outputs.
            if (message.dump().size() < 65536 && received_tools.dump().size() < 65536) {
                received_tools.push_back({{"id", call_id}, {"content", message.value("content", json())}});
            } else {
                received_tools = json::array();
            }
            (state.pending_slot == kWorkerSlot ? state.worker : state.orchestrator)
                .push_back(message);
        } else if (role == "user") {
            if (state.worker_active || state.pending_slot >= 0) {
                // Steering prompts arriving while a worker is finishing are
                // delivered to the orchestrator AFTER the private handoff.
                state.queued_user_messages.push_back(message);
            } else {
                state.orchestrator.push_back(message);
            }
        } else {
            throw std::runtime_error("Unexpected external message role: " + role);
        }
        state.visible.push_back(message);
    }
    if (!state.pending_tool_ids.empty()) {
        throw std::runtime_error("Not all pending tool results arrived in this request");
    }
    if (pending_before == kWorkerSlot && !received_tools.empty()) {
        // Only identical *tool names, semantic arguments and result texts*
        // count. Reusing a tool name or checking a changed file is fine.
        json group = json::array();
        for (auto it = state.visible.rbegin(); it != state.visible.rend(); ++it) {
            if (it->value("role", "") != "assistant" || !it->contains("tool_calls")) continue;
            for (const auto& call : (*it)["tool_calls"]) {
                const auto& fn = call["function"];
                group.push_back({{"name", fn.value("name", "")},
                    {"args", hybrid_history::canonical_tool_arguments(fn["arguments"])}});
            }
            break;
        }
        if (!group.empty()) {
            json responses = json::array();
            for (const auto& item : received_tools) {
                responses.push_back(item.value("content", json()));
            }
            const std::string signature = json{{"calls", group}, {"results", responses}}.dump();
            state.repeated_tool_rounds = signature == state.last_tool_round
                ? state.repeated_tool_rounds + 1 : 1;
            state.last_tool_round = signature;
            if (state.repeated_tool_rounds >= 10) {
                throw HybridOutputIncomplete("Hybrid worker repeated the same tool calls "
                    "with identical results ten times; stopping a probable tool loop");
            }
        }
    }
    state.pending_slot = -1;
}

void save_external(Session& state, const json& message, int slot_id) {
    const json outgoing = external_assistant(message);
    state.visible.push_back(outgoing);
    state.pending_slot = -1;
    state.pending_tool_ids.clear();
    if (outgoing.contains("tool_calls")) {
        state.pending_slot = slot_id;
        for (const auto& call : outgoing["tool_calls"]) {
            const std::string id = call.value("id", "");
            if (id.empty() || state.used_tool_ids.count(id) ||
                !state.pending_tool_ids.insert(id).second) {
                throw std::runtime_error("Backend produced empty or duplicate tool-call IDs");
            }
            state.used_tool_ids.insert(id);
        }
    }
}

// This validation happens BEFORE any external tool call is emitted or
// persisted. It prevents a bad JSON string from bricking the next Pi round.
void validate_outgoing_tools(const json& calls,
                             const std::set<std::string>& allowed,
                             const Session& state) {
    if (!calls.is_array()) throw HybridOutputIncomplete("Malformed tool-call list");
    std::set<std::string> ids;
    for (const json& call : calls) {
        if (!call.is_object() || !call.contains("function") ||
            !call["function"].is_object()) {
            throw HybridOutputIncomplete("Malformed external tool call");
        }
        const json& f = call["function"];
        const std::string id = call.value("id", "");
        const std::string name = f.value("name", "");
        if (id.empty() || !ids.insert(id).second || state.used_tool_ids.count(id) ||
            !allowed.count(name) || !f.contains("arguments") ||
            !f["arguments"].is_string()) {
            throw HybridOutputIncomplete("Invalid external tool name, ID or arguments");
        }
        if (f["arguments"].get_ref<const std::string&>().size() > kMaxToolArgumentsBytes) {
            throw HybridOutputIncomplete("External tool arguments exceed safety limit");
        }
        const auto parsed = json::parse(f["arguments"].get<std::string>(), nullptr, false);
        if (parsed.is_discarded()) {
            throw HybridOutputIncomplete("Invalid JSON in external tool arguments");
        }
    }
}

json complete_response(const json& upstream, const json& message,
                       const Session& state, const json& request,
                       const std::array<SlotPressure, 2>& slot_contexts,
                       int64_t prompt_before, int64_t output_before,
                       const hybrid_usage::Totals& perf_before, uint64_t resets_before) {
    json answer = upstream;
    answer["model"] = request.value("model", "");
    const json outgoing = external_assistant(message);
    answer["choices"][0]["message"] = outgoing;
    const std::string reason = upstream["choices"][0].value("finish_reason", "stop");
    answer["choices"][0]["finish_reason"] = outgoing.contains("tool_calls") ? "tool_calls" :
        (reason == "length" ? "length" : "stop");
    const int64_t prompt_cost = state.billed_prompt - prompt_before;
    const int64_t output_cost = state.billed_output - output_before;
    answer["usage"] = hybrid_usage::request_usage(
        prompt_cost, output_cost, state.perf, perf_before);
    answer["x_lemonade_hybrid"] = {
        {"mode", "orchestrator_worker"},
        {"context_management", "server"},
        {"worker_single_pass", true},
        {"outer_messages", state.visible.size()},
        {"orchestrator_messages", state.orchestrator.size()},
        {"worker_messages", state.worker.size()},
        {"orchestrator_prompt_tokens", state.orchestrator_prompt},
        {"worker_prompt_tokens", state.worker_prompt},
        {"internal_prompt_tokens_total", state.billed_prompt},
        {"internal_completion_tokens_total", state.billed_output},
        {"worker_generations", state.delegations},  // legacy: delegation count
        {"delegations_total", state.delegations},
        {"orchestrator_model_calls_this_request", state.perf.orchestrator.calls - perf_before.orchestrator.calls},
        {"worker_model_calls_this_request", state.perf.worker.calls - perf_before.worker.calls},
        {"worker_slot_resets_this_request", state.worker_resets - resets_before},
        {"worker_context_rollovers_total", state.worker_rollovers},
        {"orchestrator_context_rollovers_total", state.orchestrator_rollovers},
        {"context_checkpoint_model_calls_total", state.checkpoint_calls},
        {"performance", hybrid_usage::request_diagnostics(state.perf, perf_before)},
    };
    // Real per-slot n_ctx from /slots, not ctx_size or external transcript size.
    // Last measured prompt includes cached tokens and schemas, but is NOT current
    // n_past and cannot predict the next request or its completion budget.
    const auto role_context = [&](int slot) -> json {
        const int64_t last = state.role_observations[slot].prompt_tokens;
        return {{"capacity_tokens", slot_contexts[slot].context},
                {"last_prompt_tokens", last > 0 ? json(last) : json(nullptr)},
                {"measurement", last > 0 ? "last_completed_inference" : "unavailable"}};
    };
    answer["x_lemonade_hybrid"]["context"] = {
        {"management", "server"},
        {"outer_transcript_is_model_context", false},
        {"outer_transcript_limit_bytes", kMaxExternalHistoryBytes},
        {"orchestrator", role_context(kOrchestratorSlot)},
        {"worker", role_context(kWorkerSlot)},
    };
    return answer;
}

json routing_tool(const std::string& name, const std::string& description,
                  const std::string& parameter = "") {
    json properties = json::object();
    json required = json::array();
    if (!parameter.empty()) {
        properties[parameter] = { {"type", "string"} };
        required.push_back(parameter);
    }
    return {{"type", "function"},
            {"function", {{"name", name}, {"description", description},
                          {"parameters", {{"type", "object"}, {"properties", properties},
                                          {"required", required},
                                          {"additionalProperties", false}}}}}};
}

json prepare_request(const json& outer_request, const json& history,
                     int slot_id, const json& tools) {
    json request = outer_request;
    request.erase("stream_options");
    request["stream"] = false;
    request["id_slot"] = slot_id;
    request["cache_prompt"] = true;
    request["messages"] = history;
    if (tools.empty()) {
        request.erase("tools");
        request.erase("tool_choice");
    } else {
        request["tools"] = tools;
    }
    return request;
}

void record_model_usage(const json& response, bool orchestrator, bool routing,
                        double wall_ms, Session& state) {
    const hybrid_usage::Sample sample = hybrid_usage::extract(response);
    state.billed_prompt += sample.prompt;
    state.billed_output += sample.output;
    if (orchestrator) {
        state.orchestrator_prompt = sample.prompt;
    } else {
        state.worker_prompt = sample.prompt;
    }
    state.perf.add(sample, routing, wall_ms, orchestrator);
    if (trace_inference_metrics()) {
        LOG(INFO, "HybridMetrics")
            << "role=" << (orchestrator ? "orchestrator" : "worker")
            << " kind=" << (routing ? "routing" : "reply")
            << " prompt=" << sample.prompt
            << " output=" << sample.output
            << " cached=" << (sample.cached ? std::to_string(*sample.cached) : "unknown")
            << " evaluated=" << (sample.evaluated ? std::to_string(*sample.evaluated) : "unknown")
            << " prefill_ms=" << (sample.prefill_ms ? std::to_string(*sample.prefill_ms) : "unknown")
            << " wall_ms=" << wall_ms << std::endl;
    }
}

// The model's usage reports the fully rendered prompt. Keep the corresponding
// transcript footprint so subsequent tool output can be counted conservatively
// without a new inference or a tokenizer request on every normal tool round.
void observe_role_prompt(Session& state, bool orchestrator, const json& request) {
    const int slot = orchestrator ? kOrchestratorSlot : kWorkerSlot;
    const int64_t prompt = orchestrator ? state.orchestrator_prompt : state.worker_prompt;
    if (prompt <= 0 || !request.contains("messages") ||
        !request["messages"].is_array()) return;
    const json tool_defs = request.value("tools", json::array());
    state.role_observations[slot] = {prompt,
                                      request["messages"].dump().size() + tool_defs.dump().size(),
                                      request["messages"].size()};
}

json call_model(ServerContext& ctx, const json& request, bool orchestrator,
                bool routing, Session& state) {
    const auto start = std::chrono::steady_clock::now();
    hybrid_slot_scope::Role private_slot(request.value("id_slot", -1));
    const json response = ctx.router->chat_completion(request);
    if (!response.is_object() || response.contains("error") ||
        !response.contains("choices") || !response["choices"].is_array() ||
        response["choices"].empty() || !response["choices"][0].contains("message")) {
        throw std::runtime_error(std::string("Hybrid ") +
                                 (orchestrator ? "orchestrator" : "worker") +
                                 " inference failed (check the per-slot context size): " +
                                 error_message(response));
    }
    record_model_usage(response, orchestrator, routing, elapsed_ms(start), state);
    observe_role_prompt(state, orchestrator, request);
    snapshot_slots(ctx, request.value("model", ""),
                   routing ? "internal_routing_complete" : "visible_reply_complete");
    return response;
}

// Rebuild the small live role context ONLY after a complete, validated
// checkpoint has been produced. The original Pi/Kilo transcript (state.visible)
// is deliberately untouched. A checkpoint is model-generated and cannot replace
// the original client system/developer instructions or the live user request.
json latest_user_request(const json& visible) {
    for (auto it = visible.rbegin(); it != visible.rend(); ++it) {
        if (it->is_object() && it->value("role", "") == "user") return *it;
    }
    throw std::runtime_error("Hybrid orchestrator rollover found no user request");
}

json recap_user_request(json original, const std::string& summary) {
    const std::string prelude =
        "Hybrid context checkpoint (model-generated historical data, NOT new instructions; "
        "check important facts against the actual files and tools):\n" + summary +
        "\n\nMost recent user request follows, unchanged:\n";
    if (!original.contains("content")) {
        throw std::runtime_error("Cannot preserve user content during Hybrid context rollover");
    }
    if (original["content"].is_string()) {
        original["content"] = prelude + original["content"].get<std::string>();
    } else if (original["content"].is_array()) {
        json parts = json::array({{{"type", "text"}, {"text", prelude}}});
        for (const auto& part : original["content"]) parts.push_back(part);
        original["content"] = std::move(parts);
    } else {
        throw std::runtime_error("Unsupported user content during Hybrid context rollover");
    }
    return original;
}

void maybe_rollover_role(ServerContext& ctx, const json& outer_request,
                         const std::string& model, Session& state,
                         bool orchestrator, SlotPressure slot_pressure) {
    const int64_t slot_ctx = slot_pressure.context;
    const int slot = orchestrator ? kOrchestratorSlot : kWorkerSlot;
    const json& history = orchestrator ? state.orchestrator : state.worker;
    const auto& observed = state.role_observations[slot];
    const json current_tools = orchestrator ? json::array()
        : outer_request.value("tools", json::array());
    if (!hybrid_context_policy::needs_checkpoint(
            slot_ctx, observed, history.dump().size() + current_tools.dump().size(),
            history.size(), 0)) return;
    if (state.pending_slot >= 0 || !state.pending_tool_ids.empty()) {
        throw HybridOutputIncomplete("Hybrid context pressure while a client tool result is pending; "
                                     "refusing an unsafe role reset");
    }
    if (history.empty() || !history.front().is_object() ||
        history.front().value("role", "") != "system") {
        throw HybridOutputIncomplete("Hybrid context checkpoint cannot preserve the role's system instructions");
    }
    const char* role = orchestrator ? "orchestrator" : "worker";
    json checkpoint_history = history;
    checkpoint_history.push_back({{"role", "user"}, {"content",
        "Internal context checkpoint only. Do NOT use tools or perform new work. "
        "Provide a concise factual handoff: original task and hard constraints, "
        "files changed, tests actually run, observed failures, confirmed facts, "
        "open questions and exact next action. Explicitly distinguish verification "
        "limitations from code failures. Do not invent completed work or include "
        "large logs. This summary is for resuming the same role, not a final answer."}});
    json checkpoint_request = prepare_request(outer_request, checkpoint_history,
                                              slot, json::array());
    checkpoint_request.erase("response_format");
    checkpoint_request.erase("stop");
    checkpoint_request.erase("enable_thinking");
    checkpoint_request.erase("thinking");
    checkpoint_request["reasoning_effort"] = "none";
    if (!checkpoint_request.contains("chat_template_kwargs") ||
        !checkpoint_request["chat_template_kwargs"].is_object()) {
        checkpoint_request["chat_template_kwargs"] = json::object();
    }
    checkpoint_request["chat_template_kwargs"]["enable_thinking"] = false;
    // Output budget is derived from the current backend slot, not an assigned
    // or hardcoded role context size. Reserve room for this checkpoint itself.
    checkpoint_request["max_tokens"] = std::max<int64_t>(256,
        std::min<int64_t>(3072, slot_ctx / 10));
    checkpoint_request.erase("max_completion_tokens");

    json response;
    try {
        response = call_model(ctx, checkpoint_request, orchestrator, false, state);
    } catch (const std::exception& e) {
        throw HybridOutputIncomplete(std::string("Hybrid ") + role +
            " could not checkpoint before context exhaustion: " + e.what());
    }
    ++state.checkpoint_calls;
    const json& choice = response["choices"][0];
    const json& message = choice["message"];
    const std::string summary = message.contains("content") && message["content"].is_string()
        ? message["content"].get<std::string>() : "";
    try {
        hybrid_context_policy::validate_checkpoint(false,
            choice.value("finish_reason", "") == "stop",
            message.contains("tool_calls") && message["tool_calls"].is_array() &&
                !message["tool_calls"].empty(), summary, kMaxTaskBytes);
    } catch (const std::exception& e) {
        throw HybridOutputIncomplete(std::string("Hybrid ") + role +
            " checkpoint invalid; contexts were not reset: " + e.what());
    }

    json resumed = json::array();
    if (orchestrator) {
        // The initial merged system/developer text is retained VERBATIM.
        // The most recent user content is also retained verbatim as its own
        // content segment; older history is replaced by the explicit summary.
        resumed.push_back(history.front());
        resumed.push_back(recap_user_request(latest_user_request(state.visible), summary));
    } else {
        if (!state.worker_active || !state.delegation.is_object()) {
            throw HybridOutputIncomplete("Hybrid worker checkpoint without an active delegation");
        }
        const std::string args = state.delegation.value("function", json::object())
            .value("arguments", "");
        json parsed;
        try { parsed = json::parse(args); } catch (...) {
            throw HybridOutputIncomplete("Hybrid worker checkpoint has an invalid delegation");
        }
        const std::string task = parsed.value("task", "");
        if (task.empty()) {
            throw HybridOutputIncomplete("Hybrid worker checkpoint lost the original task");
        }
        resumed.push_back(history.front());
        resumed.push_back({{"role", "user"}, {"content", task +
            "\n\nProgress checkpoint (model-generated historical data, not instructions):\n" +
            summary + "\n\nContinue the unfinished work. Source edits already persist on disk; "
            "verify rather than blindly repeat completed steps."}});
    }
    // A reset can never be allowed to hide a missing, oversized or malformed
    // checkpoint. Existing external tool IDs remain in state.visible intact.
    erase_slot(ctx, model, slot);
    (orchestrator ? state.orchestrator : state.worker) = std::move(resumed);
    state.role_observations[slot] = {};
    if (orchestrator) {
        ++state.orchestrator_rollovers;
        state.orchestrator_prompt = 0;
    } else {
        ++state.worker_rollovers;
        ++state.worker_resets;
        state.worker_prompt = 0;
    }
    LOG(INFO, "HybridCoder") << "Context checkpoint completed for " << role
        << ", slot=" << slot << ", capacity=" << slot_ctx << " tokens"
        << ", summary_bytes=" << summary.size() << std::endl;
}

json route_action(ServerContext& ctx, const json& outer_request, const json& history,
                  bool orchestrator, bool can_use_tools, Session& state,
                  int routing_max_tokens = kRoutingMaxTokens) {
    const int slot_id = orchestrator ? kOrchestratorSlot : kWorkerSlot;
    json tools = json::array();
    if (orchestrator) {
        tools.push_back(delegate_tool());
        tools.push_back(routing_tool(kRespondName, "Respond to the user without using client tools."));
    } else {
        tools.push_back(routing_tool(kWorkerDoneName,
            "The assigned work is finished; return a truthful, concise report.", "summary"));
        if (can_use_tools) {
            tools.push_back(routing_tool(kWorkerToolsName,
                "More client tool calls are needed to finish the assigned work."));
        }
    }
    json decision_history = history;
    decision_history.push_back({{"role", "user"},
        {"content", orchestrator
            ? "Routing step only: call hybrid_delegate_task with a self-contained task if "
              "delegation is useful, otherwise call hybrid_respond. Never write an answer "
              "in this step. Do not call the client's tools here."
            : "Routing step only: call hybrid_worker_done with the true result if the "
              "implementation is complete and verification succeeded or is blocked by "
              "missing test dependencies or infrastructure (report the blocker). "
              "Otherwise call hybrid_worker_use_tools to request client tools. "
              "Do not invoke a client tool in this step."}});
    json request = prepare_request(outer_request, decision_history, slot_id, tools);
    request["tool_choice"] = "required";
    request["max_tokens"] = routing_max_tokens;
    request.erase("max_completion_tokens");
    request.erase("response_format");
    request.erase("stop");
    request.erase("enable_thinking");
    request.erase("thinking");
    request["reasoning_effort"] = "none";
    if (!request.contains("chat_template_kwargs") ||
        !request["chat_template_kwargs"].is_object()) {
        request["chat_template_kwargs"] = json::object();
    }
    request["chat_template_kwargs"]["enable_thinking"] = false;
    const json response = call_model(ctx, request, orchestrator, true, state);
    const json usage = response.contains("usage") && response["usage"].is_object()
        ? response["usage"] : json::object();
    if (response["choices"][0].value("finish_reason", "") == "length") {
        throw RoutingIncomplete(std::string("Hybrid ") +
            (orchestrator ? "orchestrator" : "worker") +
            " routing hit its " + std::to_string(routing_max_tokens) +
            "-token output limit (prompt=" +
            std::to_string(usage.value("prompt_tokens", int64_t{0})) +
            ", output=" + std::to_string(usage.value("completion_tokens", int64_t{0})) +
            "). Increase ctx_size if the per-slot window is full.", true);
    }
    const json message = external_assistant(response["choices"][0]["message"]);
    const json calls = message.value("tool_calls", json::array());
    if (calls.size() != 1 || !calls[0].is_object()) {
        throw RoutingIncomplete("Hybrid routing returned no single complete internal tool call");
    }
    const std::string name = calls[0].value("function", json::object()).value("name", "");
    if (orchestrator ? (name != kDelegateName && name != kRespondName)
                     : (name != kWorkerDoneName && name != kWorkerToolsName)) {
        throw RoutingIncomplete("Hybrid routing returned an unexpected tool: " + name);
    }
    if (name == kWorkerToolsName && !can_use_tools) {
        throw std::runtime_error("Hybrid worker requested tools that the client did not expose");
    }
    return calls[0];
}

json model_reply(ServerContext& ctx, const json& outer_request, const json& history,
                 int slot_id, bool orchestrator, bool require_tools, Session& state) {
    json tools = orchestrator ? json::array() : tool_list(outer_request, false);
    if (!orchestrator && require_tools) {
        tools.push_back(routing_tool(kWorkerDoneName,
            "Finish the assigned task with an accurate summary of changes, tests and blockers.",
            "summary"));
    }
    json request = prepare_request(outer_request, history, slot_id, tools);
    // Preserve the client's tool_choice (usually "auto"). Requiring a tool at
    // every Worker turn prevents normal textual completion and biases the model
    // toward another external call even when verification is complete.
    const json response = call_model(ctx, request, orchestrator, false, state);
    const json& choice = response["choices"][0];
    const json& message = choice["message"];
    const bool has_calls = message.contains("tool_calls") &&
        message["tool_calls"].is_array() && !message["tool_calls"].empty();
    if (choice.value("finish_reason", "") == "length" && (require_tools || has_calls)) {
        throw HybridOutputIncomplete("Hybrid tool generation ended at the output/context limit; "
                                 "refusing to forward incomplete tool arguments");
    }
    if (orchestrator && hybrid_worker_policy::unusable_visible_reply(
            message.value("content", json()).is_string() &&
                !message["content"].get<std::string>().empty(), has_calls)) {
        throw HybridOutputIncomplete("Hybrid visible reply produced no text or tool calls "
                                 "(finish_reason=" + choice.value("finish_reason", "unknown") +
                                 "); refusing an empty successful completion");
    }
    return response;
}

struct StreamEmitter {
    httplib::DataSink& sink;
    std::string id;
    std::string model;
    long created;
    bool started = false;
    bool disconnected = false;

    StreamEmitter(httplib::DataSink& target, const std::string& requested_model)
        : sink(target),
          id("chatcmpl-hybrid-" + std::to_string(static_cast<long>(std::time(nullptr))) +
             "-" + std::to_string(next_id())),
          model(requested_model), created(static_cast<long>(std::time(nullptr))) {}

    static uint64_t next_id() {
        static std::atomic<uint64_t> counter{0};
        return counter.fetch_add(1);
    }

    bool send(const json& chunk) {
        if (disconnected || (sink.is_writable && !sink.is_writable())) {
            disconnected = true;
            return false;
        }
        const std::string frame = "data: " + chunk.dump() + "\n\n";
        if (!sink.write(frame.data(), frame.size())) {
            disconnected = true;
            return false;
        }
        return true;
    }

    json chunk(const json& delta, const json& finish_reason) const {
        return {{"id", id}, {"object", "chat.completion.chunk"},
                {"created", created}, {"model", model},
                {"choices", json::array({{{"index", 0}, {"delta", delta},
                                           {"finish_reason", finish_reason}}})}};
    }

    bool emit_delta(const json& delta) {
        if (!started) {
            if (!send(chunk({{"role", "assistant"}}, nullptr))) return false;
            started = true;
        }
        return send(chunk(delta, nullptr));
    }

    void complete(const json& answer, bool include_usage) {
        if (!started && !emit_delta(json::object())) {
            throw std::runtime_error("Hybrid stream client disconnected");
        }
        json final = chunk(json::object(), answer["choices"][0]["finish_reason"]);
        final["x_lemonade_hybrid"] = answer["x_lemonade_hybrid"];
        if (!send(final)) throw std::runtime_error("Hybrid stream client disconnected");
        if (include_usage) {
            if (!send({{"id", id}, {"object", "chat.completion.chunk"},
                       {"created", created}, {"model", model},
                       {"choices", json::array()},
                       {"usage", answer.value("usage", json::object())},
                       {"x_lemonade_hybrid", answer["x_lemonade_hybrid"]}})) {
                throw std::runtime_error("Hybrid stream client disconnected");
            }
        }
        static constexpr char done[] = "data: [DONE]\n\n";
        if (!sink.write(done, sizeof(done) - 1)) {
            disconnected = true;
            throw std::runtime_error("Hybrid stream client disconnected");
        }
        sink.done();
    }
};

struct ToolFragments {
    std::string id;
    std::string name;
    std::string arguments;
};

json model_reply_stream(ServerContext& ctx, const json& outer_request,
                        const json& history, int slot_id, bool orchestrator,
                        bool require_tools, Session& state, StreamEmitter& emitter) {
    const bool worker_step = !orchestrator && require_tools;
    json tools = orchestrator ? json::array() : tool_list(outer_request, false);
    if (worker_step) {
        tools.push_back(routing_tool(kWorkerDoneName,
            "Finish the assigned task with an accurate summary of changes, tests and blockers.",
            "summary"));
    }
    json request = prepare_request(outer_request, history, slot_id, tools);
    // Preserve the client's tool_choice (usually "auto"). Requiring a tool at
    // every Worker turn prevents normal textual completion and biases the model
    // toward another external call even when verification is complete.
    request["stream"] = true;
    request["stream_options"] = {{"include_usage", true}};
    const auto inference_start = std::chrono::steady_clock::now();

    std::set<std::string> external_tool_names;
    for (const auto& tool : tools) {
        const std::string name = tool.value("function", json::object()).value("name", "");
        if (!name.empty() && name != kWorkerDoneName) external_tool_names.insert(name);
    }
    // Worker fragments stay private until the entire tool-call group is
    // classified; emit compact complete calls only after validation.
    std::string buffer;
    std::string content;
    std::string reasoning;
    std::string failure;
    std::map<int, ToolFragments> tool_fragments;
    json upstream_usage = json::object();
    json timings = json::object();
    std::string finish_reason;
    bool saw_done = false;

    auto on_line = [&](const std::string& line) {
        if (!line.empty() && line[0] == ':') {
            // Keep the client connection alive while the worker is choosing
            // a tool or its private done action, without leaking its contents.
            if (worker_step) {
                static constexpr char ping[] = ": ping\n\n";
                if (!emitter.sink.write(ping, sizeof(ping) - 1)) {
                    emitter.disconnected = true;
                    failure = "Hybrid stream client disconnected";
                }
            }
            return;
        }
        if (line.compare(0, 5, "data:") != 0) return;
        std::string payload = line.substr(5);
        if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
        if (payload == "[DONE]") {
            saw_done = true;
            return;
        }
        if (!failure.empty()) return;
        json chunk;
        try {
            chunk = json::parse(payload);
        } catch (const std::exception& e) {
            failure = std::string("Malformed llama.cpp SSE payload: ") + e.what();
            return;
        }
        if (!chunk.is_object()) return;
        if (chunk.contains("error")) {
            failure = error_message(chunk);
            return;
        }
        if (chunk.contains("usage") && chunk["usage"].is_object()) {
            upstream_usage = chunk["usage"];
        }
        if (chunk.contains("timings") && chunk["timings"].is_object()) {
            timings = chunk["timings"];
        }
        if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
            chunk["choices"].empty()) return;
        const json& choice = chunk["choices"][0];
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
            finish_reason = choice["finish_reason"].get<std::string>();
        }
        if (!choice.contains("delta") || !choice["delta"].is_object()) return;
        const json& delta = choice["delta"];
        json outward = json::object();
        if (delta.contains("content") && delta["content"].is_string()) {
            const std::string fragment = delta["content"].get<std::string>();
            content += fragment;
            if (orchestrator) outward["content"] = fragment;
        }
        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
            const std::string fragment = delta["reasoning_content"].get<std::string>();
            reasoning += fragment;
            if (orchestrator) outward["reasoning_content"] = fragment;
        }
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto& fragment : delta["tool_calls"]) {
                if (!fragment.is_object() || !fragment.contains("index") ||
                    !fragment["index"].is_number_integer()) {
                    failure = "Malformed streamed tool-call fragment";
                    return;
                }
                const int index = fragment["index"].get<int>();
                if (index < 0 || index > 64) {
                    failure = "Streamed tool-call index is out of range";
                    return;
                }
                ToolFragments& tool = tool_fragments[index];
                if (fragment.contains("id") && fragment["id"].is_string()) {
                    const std::string id = fragment["id"].get<std::string>();
                    if (!tool.id.empty() && tool.id != id) {
                        failure = "Streamed tool-call ID changed during generation";
                        return;
                    }
                    tool.id = id;
                }
                if (fragment.contains("function") && fragment["function"].is_object()) {
                    const json& f = fragment["function"];
                    if (f.contains("name") && f["name"].is_string()) {
                        tool.name += f["name"].get<std::string>();
                    }
                    if (f.contains("arguments") && f["arguments"].is_string()) {
                        tool.arguments += f["arguments"].get<std::string>();
                    }
                }
                if (tool.arguments.size() > kMaxToolArgumentsBytes ||
                    !hybrid_worker_policy::possible_name_prefix(
                        tool.name, external_tool_names, kWorkerDoneName)) {
                    failure = "Hybrid streamed an oversized or undeclared tool call";
                    return;
                }
            }
        }
        if (!outward.empty() && !emitter.emit_delta(outward)) {
            failure = "Hybrid stream client disconnected";
        }
    };

    httplib::DataSink proxy;
    proxy.write = [&](const char* data, size_t length) -> bool {
        if (!failure.empty() || emitter.disconnected) return false;
        if (buffer.size() + length > kMaxSseLineBytes) {
            failure = "Hybrid backend sent an oversized SSE line";
            return false;
        }
        buffer.append(data, length);
        StreamingProxy::process_sse_lines(buffer, on_line);
        return failure.empty() && !emitter.disconnected;
    };
    proxy.is_writable = [&]() {
        return !emitter.disconnected &&
               (!emitter.sink.is_writable || emitter.sink.is_writable());
    };
    proxy.done = []() {};
    proxy.done_with_trailer = [](const httplib::Headers&) {};

    hybrid_slot_scope::Role private_slot(request.value("id_slot", -1));
    ctx.router->chat_completion_stream(request.dump(), proxy);
    StreamingProxy::process_sse_lines(buffer, on_line, true);
    if (!failure.empty()) throw std::runtime_error(failure);
    if (emitter.disconnected || (emitter.sink.is_writable && !emitter.sink.is_writable())) {
        throw std::runtime_error("Hybrid stream client disconnected");
    }
    if (!saw_done || (finish_reason != "stop" && finish_reason != "tool_calls" &&
                      finish_reason != "length")) {
        throw std::runtime_error("Hybrid backend stream did not complete successfully");
    }
    if (finish_reason == "length" && (require_tools || !tool_fragments.empty())) {
        throw HybridOutputIncomplete("Hybrid streamed tool generation hit its output/context "
                                 "limit; refusing incomplete tool arguments");
    }
    json message = {{"role", "assistant"}, {"content", content}};
    if (!tool_fragments.empty()) {
        message["tool_calls"] = json::array();
        std::vector<std::string> names;
        std::vector<std::string> ids;
        for (const auto& [index, fragments] : tool_fragments) {
            if (fragments.id.empty() || fragments.name.empty()) {
                throw std::runtime_error("Invalid hybrid streamed tool call");
            }
            names.push_back(fragments.name);
            ids.push_back(fragments.id);
            message["tool_calls"].push_back({{"id", fragments.id}, {"type", "function"},
                {"function", {{"name", fragments.name},
                              {"arguments", fragments.arguments}}}});
        }
        const bool private_done = worker_step &&
            hybrid_worker_policy::classify(names, kWorkerDoneName) ==
                hybrid_worker_policy::Outcome::Done;
        if (private_done) {
            const auto& done = message["tool_calls"][0];
            if (done.value("id", "").empty() ||
                !done["function"].contains("arguments") ||
                !done["function"]["arguments"].is_string() ||
                json::parse(done["function"]["arguments"].get<std::string>(),
                            nullptr, false).is_discarded()) {
                throw HybridOutputIncomplete("Malformed private worker completion");
            }
        } else {
            validate_outgoing_tools(message["tool_calls"], external_tool_names, state);
            size_t index = 0;
            for (const auto& [ordinal, tool] : tool_fragments) {
                json delta = message["tool_calls"][index++];
                delta["index"] = ordinal;
                if (!emitter.emit_delta({{"tool_calls", json::array({delta})}})) {
                    throw HybridOutputIncomplete("Hybrid stream disconnected before tool release");
                }
            }
        }
    }
    if (worker_step && tool_fragments.empty()) {
        // Hidden worker text is a legitimate handoff, but only on a clean
        // stop. Nothing has been emitted to the client, so the orchestrator
        // can consume it without exposing internal instructions.
        try {
            hybrid_worker_policy::classify_reply({}, kWorkerDoneName, content, finish_reason);
        } catch (const std::runtime_error& e) {
            throw HybridOutputIncomplete(e.what());
        }
    }
    if (orchestrator && hybrid_worker_policy::unusable_visible_reply(
            !content.empty(), !tool_fragments.empty())) {
        throw HybridOutputIncomplete("Hybrid visible reply produced no text or tool calls "
                                 "(finish_reason=" + finish_reason +
                                 "); refusing an empty successful completion");
    }
    if (!upstream_usage.contains("prompt_tokens") && timings.contains("prompt_n")) {
        upstream_usage["prompt_tokens"] = timings["prompt_n"];
    }
    if (!upstream_usage.contains("completion_tokens") && timings.contains("predicted_n")) {
        upstream_usage["completion_tokens"] = timings["predicted_n"];
    }
    const int64_t prompt = upstream_usage.value("prompt_tokens", int64_t{0});
    const int64_t output = upstream_usage.value("completion_tokens", int64_t{0});
    json response = {{"id", emitter.id}, {"object", "chat.completion"},
                     {"created", emitter.created}, {"model", emitter.model},
                     {"choices", json::array({{{"index", 0}, {"message", message},
                                                {"finish_reason", finish_reason}}})},
                     {"usage", {{"prompt_tokens", prompt},
                                 {"completion_tokens", output},
                                 {"total_tokens", prompt + output}}}};
    if (!timings.empty()) response["timings"] = timings;
    if (upstream_usage.contains("prompt_tokens_details") &&
        upstream_usage["prompt_tokens_details"].is_object()) {
        response["usage"]["prompt_tokens_details"] = upstream_usage["prompt_tokens_details"];
    }
    if (upstream_usage.contains("input_tokens_details") &&
        upstream_usage["input_tokens_details"].is_object()) {
        response["usage"]["input_tokens_details"] = upstream_usage["input_tokens_details"];
    }
    record_model_usage(response, orchestrator, false, elapsed_ms(inference_start), state);
    observe_role_prompt(state, orchestrator, request);
    snapshot_slots(ctx, request.value("model", ""), "visible_reply_complete");
    if (!reasoning.empty()) response["choices"][0]["message"]["reasoning_content"] = reasoning;
    return response;
}

// An explicit ID is required for continuation across compaction or rewinds.
// Without one, the first *actual user request* and preceding instructions
// identify a session deterministically for conventional OpenAI clients.
// Collisions cannot silently merge changed histories: ingest() rechecks them.
std::string hybrid_session_key(const std::string& backend, const json& request,
                               const json& incoming) {
    const json explicit_id = request.value("x_lemonade_hybrid_session", json(""));
    if (!explicit_id.is_string()) {
        throw std::runtime_error("x_lemonade_hybrid_session must be a string");
    }
    const std::string id = explicit_id.get<std::string>();
    if (!id.empty()) {
        if (id.size() > 128) throw std::runtime_error("Hybrid session ID is too long");
        return backend + ":session:" + id;
    }
    json initial = json::array();
    for (const auto& msg : incoming) {
        if (!msg.is_object()) break;
        const auto role = msg.value("role", "");
        if (role != "system" && role != "developer" && role != "user") break;
        initial.push_back(hybrid_history::canonical_history_message(msg));
        if (role == "user") break;
    }
    return backend + ":initial:" + std::to_string(std::hash<std::string>{}(initial.dump()));
}

// A failed inference may change the backend KV cache without committing the
// corresponding role history. Invalidate only this model's private cache lease.
// Caller holds the requested model's SessionStore mutex.
void invalidate_active_backend(ServerContext& ctx, const json& request,
                               SessionStore& store) noexcept {
    try {
        const std::string model = request.value("model", std::string());
        if (!model.empty()) store.active_sessions.erase(backend_identity(ctx, model));
    } catch (const std::exception&) {
        // Backend may already have crashed or been unloaded. A newly loaded
        // instance has a different identity, so old leases cannot be reused.
    }
}

json run_locked(ServerContext& ctx, const json& request, SessionStore& store,
                StreamEmitter* emitter = nullptr) {
    auto& g_sessions = store.sessions;
    auto& g_active_session = store.active_sessions;
    const std::string model = request.value("model", "");
    const json incoming = request.value("messages", json::array());
    if (model.empty() || !incoming.is_array() || incoming.empty()) {
        throw std::runtime_error("Hybrid model and messages are required");
    }
    if (request.value("n", 1) != 1) {
        throw std::runtime_error("Hybrid-Coder-Mode supports one completion per request");
    }
    // Bound outer-history copies without touching normal Chat API traffic.
    if (incoming.dump().size() > kMaxExternalHistoryBytes) {
        throw std::runtime_error("Hybrid external transcript exceeds 32 MiB; "
                                 "compact using the stable session ID");
    }
    const std::string identity = backend_identity(ctx, model);
    const uint64_t ordinary_generation = hybrid_slot_scope::ordinary_generation(model);
    auto slot_contexts = check_slots(ctx, model);

    const std::string session_key = hybrid_session_key(identity, request, incoming);
    const auto existing = g_sessions.find(session_key);
    Session state;
    if (existing == g_sessions.end()) {
        state.backend_identity = identity;
        initialize(state, ctx, incoming, model);
        g_active_session[identity] = session_key;
        store.observed_ordinary_generation[identity] = ordinary_generation;
    } else {
        state = existing->second;
        // Validate the complete incoming client history *before* mutating KV.
        ingest(state, incoming,
               !request.value("x_lemonade_hybrid_session", std::string()).empty());
        if (g_active_session[identity] != session_key ||
            store.observed_ordinary_generation[identity] != ordinary_generation) {
            // The two slots may contain an unrelated client's KV. Rebuild
            // from the role histories; never checkpoint its foreign n_past.
            erase_slot(ctx, model, kOrchestratorSlot);
            erase_slot(ctx, model, kWorkerSlot);
            state.role_observations = {};
            g_active_session[identity] = session_key;
            store.observed_ordinary_generation[identity] = ordinary_generation;
        }
    }
    // Physical slots may have been erased while switching sessions.
    // A stale n_past from another session can trigger a false checkpoint.
    slot_contexts = check_slots(ctx, model);
    const int64_t prompt_before = state.billed_prompt;
    const int64_t output_before = state.billed_output;
    const hybrid_usage::Totals perf_before = state.perf;
    const uint64_t resets_before = state.worker_resets;
    const json external_tools = tool_list(request, false);
    std::set<std::string> allowed_external_tools;
    for (const auto& tool : external_tools) {
        allowed_external_tools.insert(
            tool.value("function", json::object()).value("name", ""));
    }
    const bool can_use_tools = !external_tools.empty() &&
        request.value("tool_choice", json("auto")) != "none";
    std::string routing_fallback;

    const auto hybrid_deadline = std::chrono::steady_clock::now() +
                                  std::chrono::minutes(20);
    for (int n = 0; n < kMaxInternalCalls; ++n) {
        if (std::chrono::steady_clock::now() > hybrid_deadline) {
            throw HybridOutputIncomplete("Hybrid request exceeded its 20-minute budget");
        }
        const bool orchestrator = !state.worker_active;
        const int slot = orchestrator ? kOrchestratorSlot : kWorkerSlot;
        maybe_rollover_role(ctx, request, model, state, orchestrator,
                            slot_contexts[slot]);
        const json& history = orchestrator ? state.orchestrator : state.worker;
        if (orchestrator) {
            json action;
            if (!can_use_tools) {
                // No client tools: the worker cannot usefully implement changes.
                action = {{"function", {{"name", kRespondName}}}};
                routing_fallback = "no_client_tools";
            } else {
                try {
                    action = route_action(ctx, request, history, true,
                                          can_use_tools, state);
                } catch (const RoutingIncomplete& e) {
                    LOG(WARNING, "HybridCoder") << e.what()
                        << "; proceeding with direct orchestrator response" << std::endl;
                    action = {{"function", {{"name", kRespondName}}}};
                    routing_fallback = "orchestrator_direct";
                }
            }
            const std::string action_name = action.value("function", json::object()).value("name", "");
            if (action_name == kDelegateName) {
                ++state.delegations;
                const std::string arguments = action.value("function", json::object()).value("arguments", "");
                const json parsed = json::parse(arguments);
                const std::string task = parsed.value("task", "");
                if (task.empty() || task.size() > kMaxTaskBytes ||
                    action.value("id", "").empty()) {
                    throw std::runtime_error("Hybrid delegate_task requires a bounded task and tool-call ID");
                }
                state.orchestrator.push_back({{"role", "assistant"}, {"content", ""},
                                              {"tool_calls", json::array({action})}});
                state.delegation = action;
                state.worker_active = true;
                erase_slot(ctx, model, kWorkerSlot);
                ++state.worker_resets;
                state.worker = json::array({{{"role", "system"}, {"content", kWorkerPrompt}},
                                            {{"role", "user"}, {"content", task}}});
                state.worker_prompt = 0;
                state.role_observations[kWorkerSlot] = {};
                continue;
            }
            if (action_name != kRespondName) {
                throw std::runtime_error("Invalid Hybrid orchestrator route action");
            }
        }

        const bool worker_tools = !orchestrator;
        json response = emitter
            ? model_reply_stream(ctx, request, history, slot, orchestrator,
                                 worker_tools, state, *emitter)
            : model_reply(ctx, request, history, slot, orchestrator,
                          worker_tools, state);
        const json message = external_assistant(response["choices"][0]["message"]);
        const json calls = message.value("tool_calls", json::array());
        if (worker_tools) {
            std::vector<std::string> names;
            for (const auto& call : calls) {
                names.push_back(call.value("function", json::object()).value("name", ""));
            }
            const std::string worker_text = message.contains("content") &&
                message["content"].is_string() ? message["content"].get<std::string>() : "";
            const auto outcome = hybrid_worker_policy::classify_reply(
                names, kWorkerDoneName, worker_text,
                response["choices"][0].value("finish_reason", ""));
            if (outcome != hybrid_worker_policy::Outcome::External) {
                std::string summary = worker_text;
                if (outcome == hybrid_worker_policy::Outcome::Done) {
                    const json& done = calls[0];
                    const std::string args = done.value("function", json::object()).value("arguments", "");
                    const json parsed = json::parse(args);
                    summary = parsed.value("summary", "");
                }
                if (summary.empty() || summary.size() > kMaxTaskBytes) {
                    throw std::runtime_error("Worker handoff must be concise and nonempty");
                }
                erase_slot(ctx, model, kWorkerSlot);
                ++state.worker_resets;
                state.worker = json::array();
                state.worker_prompt = 0;
                state.role_observations[kWorkerSlot] = {};
                state.worker_active = false;
                state.orchestrator.push_back({{"role", "tool"},
                                               {"tool_call_id", state.delegation.value("id", "")},
                                               {"content", summary}});
                state.delegation = nullptr;
                for (const auto& user : state.queued_user_messages) {
                    state.orchestrator.push_back(user);
                }
                state.queued_user_messages = json::array();
                continue;
            }
            std::vector<std::string> ids;
            for (const auto& call : calls) ids.push_back(call.value("id", ""));
            hybrid_worker_policy::validate_external(names, ids, allowed_external_tools);
        }
        for (const auto& call : calls) {
            const std::string name = call.value("function", json::object()).value("name", "");
            if (name == kDelegateName || name == kRespondName || name == kWorkerDoneName ||
                name == kWorkerToolsName) {
                throw std::runtime_error("Internal hybrid tool leaked into a client response");
            }
        }
        if (!calls.empty()) {
            if (orchestrator) {
                throw HybridOutputIncomplete("Orchestrator must not execute external tools");
            }
            validate_outgoing_tools(calls, allowed_external_tools, state);
        }
        json outward = message;
        if (worker_tools) outward["content"] = "";
        (orchestrator ? state.orchestrator : state.worker).push_back(message);
        save_external(state, outward, slot);
        json result = complete_response(response, outward, state, request,
                                        slot_contexts, prompt_before, output_before,
                                        perf_before, resets_before);
        if (!routing_fallback.empty()) {
            result["x_lemonade_hybrid"]["routing_fallback"] = routing_fallback;
        }
        if (emitter) {
            result["id"] = emitter->id;
            result["created"] = emitter->created;
        }
        if (trace_slot_usage()) {
            LOG(INFO, "HybridProfile") << "model=" << model
                << " request_performance="
                << result["x_lemonade_hybrid"]["performance"].dump()
                << " worker_slot_resets="
                << result["x_lemonade_hybrid"]["worker_slot_resets_this_request"]
                << std::endl;
        }
        g_sessions[session_key] = std::move(state);
        // Limit retained, private logical histories to avoid unbounded RAM.
        if (g_sessions.size() > kMaxSavedSessions) {
            for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it) {
                if (it->first != session_key) {
                    g_sessions.erase(it);
                    break;
                }
            }
        }
        return result;
    }
    throw std::runtime_error("Hybrid internal inference loop budget exceeded");
}

void write_sse(httplib::DataSink& sink, const json& payload) {
    const std::string frame = "data: " + payload.dump() + "\n\n";
    sink.write(frame.c_str(), frame.size());
}

} // namespace

bool hybrid_coder_enabled(ServerContext& ctx, const std::string& model) {
    const RecipeOptions opts = ctx.router->get_model_recipe_options(model);
    const json value = opts.get_option("hybrid_coder_mode");
    return opts.get_recipe() == "llamacpp" && value.is_boolean() && value.get<bool>();
}

json run_hybrid_coder(ServerContext& ctx, const json& request) {
    auto store = session_store_for(request.value("model", std::string()));
    std::unique_lock<std::mutex> lock(store->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return error_response("Another hybrid conversation request is still active", "hybrid_busy");
    }
    try {
        return run_locked(ctx, request, *store);
    } catch (const HybridOutputIncomplete& e) {
        LOG(ERROR, "HybridCoder") << e.what() << std::endl;
        invalidate_active_backend(ctx, request, *store); // only this backend
        return error_response(e.what(), "hybrid_output_incomplete");
    } catch (const std::exception& e) {
        LOG(ERROR, "HybridCoder") << e.what() << std::endl;
        invalidate_active_backend(ctx, request, *store); // only this backend
        return error_response(e.what(), "hybrid_context_conflict");
    }
}

void stream_hybrid_coder(ServerContext& ctx, const json& request,
                         httplib::DataSink& sink) {
    StreamEmitter emitter(sink, request.value("model", ""));
    auto store = session_store_for(request.value("model", std::string()));
    std::unique_lock<std::mutex> lock(store->mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        write_sse(sink, error_response("Another hybrid conversation request is still active",
                                       "hybrid_busy"));
        sink.done();
        return;
    }
    try {
        const json answer = run_locked(ctx, request, *store, &emitter);
        const bool include_usage = request.contains("stream_options") &&
            request["stream_options"].is_object() &&
            request["stream_options"].value("include_usage", false);
        lock.unlock();
        emitter.complete(answer, include_usage);
    } catch (const HybridOutputIncomplete& e) {
        LOG(ERROR, "HybridCoder") << e.what() << std::endl;
        // A partially streamed answer must not be resumable as a completed
        // turn. Never append [DONE] to this error response.
        // A failed generation may leave stale KV for this model; the next
        // Hybrid request will re-render its authoritative role history.
        if (!lock.owns_lock()) lock.lock();
        invalidate_active_backend(ctx, request, *store);
        lock.unlock();
        if (!emitter.disconnected) {
            write_sse(sink, error_response(e.what(), "hybrid_output_incomplete"));
        }
        sink.done();
    } catch (const std::exception& e) {
        LOG(ERROR, "HybridCoder") << e.what() << std::endl;
        if (!lock.owns_lock()) lock.lock();
        invalidate_active_backend(ctx, request, *store);
        lock.unlock();
        if (!emitter.disconnected) {
            write_sse(sink, error_response(e.what(), "hybrid_context_conflict"));
        }
        sink.done();
    }
}

} // namespace lemon
