#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "lemon/smart_thinking_verified_ir.h"

namespace lemon {

inline constexpr std::int64_t kVerifiedArithmeticLiteralLimit = 1000000000LL;
inline constexpr std::int64_t kVerifiedArithmeticStateMagnitudeLimit =
    1000000000000000LL;
inline constexpr std::size_t kVerifiedArithmeticMaxVariables = 64;
inline constexpr std::size_t kVerifiedArithmeticMaxOperations = 4096;
inline constexpr std::size_t kVerifiedArithmeticMaxChunkSize = 12;
inline constexpr std::size_t kVerifiedDispatchMaxTasks = 256;
inline constexpr std::size_t kVerifiedDispatchMaxDependencies = 4096;
inline constexpr std::size_t kVerifiedDispatchMaxWorkers = 64;
inline constexpr std::size_t kVerifiedDispatchMaxIdentifierBytes = 64;
inline constexpr std::size_t kVerifiedDispatchMaxChunkSize = 8;
inline constexpr std::int64_t kVerifiedDispatchMaxDuration = 1000000000LL;
inline constexpr std::int64_t kVerifiedDispatchMaxChecksumLiteral = 1000000000LL;

struct SmartThinkingArithmeticExecutionResult {
    bool completed = false;
    std::map<std::string, std::int64_t> variables;
    std::int64_t checksum = 0;
    std::size_t operations_executed = 0;
    std::size_t chunks_executed = 0;
    std::string failure_reason;
};

struct SmartThinkingDispatchScheduleEntry {
    int worker = 0;
    std::int64_t start = 0;
    std::int64_t finish = 0;
};

struct SmartThinkingDispatchExecutionResult {
    bool completed = false;
    std::int64_t makespan = 0;
    std::vector<std::string> completion_order;
    std::map<std::string, SmartThinkingDispatchScheduleEntry> schedule;
    std::int64_t checksum = 0;
    std::size_t completion_events = 0;
    std::size_t tasks_completed = 0;
    std::string failure_reason;
};

namespace verified_execution_detail {

inline bool checked_add(std::int64_t lhs, std::int64_t rhs,
                        std::int64_t* result) {
    if (result == nullptr) return false;
    if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
        return false;
    }
    *result = lhs + rhs;
    return true;
}

inline bool checked_sub(std::int64_t lhs, std::int64_t rhs,
                        std::int64_t* result) {
    if (rhs == std::numeric_limits<std::int64_t>::min()) return false;
    return checked_add(lhs, -rhs, result);
}

inline std::int64_t nonnegative_mod(std::int64_t value,
                                    std::int64_t modulus) {
    const auto remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
}

inline std::uint64_t add_mod(std::uint64_t lhs, std::uint64_t rhs,
                             std::uint64_t modulus) {
    return lhs >= modulus - rhs ? lhs - (modulus - rhs) : lhs + rhs;
}

inline std::uint64_t mul_mod(std::uint64_t lhs, std::uint64_t rhs,
                             std::uint64_t modulus) {
    std::uint64_t result = 0;
    while (rhs != 0) {
        if ((rhs & 1U) != 0U) result = add_mod(result, lhs, modulus);
        rhs >>= 1U;
        if (rhs != 0) lhs = add_mod(lhs, lhs, modulus);
    }
    return result;
}

inline bool modular_product_sum(std::int64_t base, std::int64_t lhs,
                                std::int64_t rhs, std::int64_t addend,
                                std::int64_t modulus, std::int64_t* result) {
    if (result == nullptr || modulus <= 0) return false;
    const auto mod = static_cast<std::uint64_t>(modulus);
    const auto residue = [modulus](std::int64_t value) {
        return static_cast<std::uint64_t>(nonnegative_mod(value, modulus));
    };
    std::uint64_t value = residue(base);
    value = add_mod(value, mul_mod(residue(lhs), residue(rhs), mod), mod);
    value = add_mod(value, residue(addend), mod);
    *result = static_cast<std::int64_t>(value);
    return true;
}

inline bool arithmetic_literal_in_range(std::int64_t value) {
    return value >= -kVerifiedArithmeticLiteralLimit &&
           value <= kVerifiedArithmeticLiteralLimit;
}

inline bool arithmetic_identifier_valid(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!(std::isalpha(first) || value.front() == '_')) return false;
    for (char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (!(std::isalnum(byte) || c == '_')) return false;
    }
    return true;
}

inline bool arithmetic_value_in_range(std::int64_t value) {
    return value >= -kVerifiedArithmeticStateMagnitudeLimit &&
           value <= kVerifiedArithmeticStateMagnitudeLimit;
}

inline bool dispatch_identifier_valid(const std::string& value) {
    if (value.empty() || value.size() > kVerifiedDispatchMaxIdentifierBytes) {
        return false;
    }
    const auto first = static_cast<unsigned char>(value.front());
    if (!(std::isalpha(first) || value.front() == '_')) return false;
    for (char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (!(std::isalnum(byte) || c == '_' || c == '-')) return false;
    }
    return true;
}

}  // namespace verified_execution_detail

inline bool validate_smart_thinking_arithmetic_ir(
    const SmartThinkingArithmeticIR& ir,
    std::string* failure_reason = nullptr) {
    const auto fail = [&](const std::string& reason) {
        if (failure_reason != nullptr) *failure_reason = reason;
        return false;
    };
    if (ir.family_id != "arithmetic_state_program" ||
        ir.contract_version != "1") {
        return fail("arithmetic_ir_contract_invalid");
    }
    if (ir.initial_variables.empty() ||
        ir.initial_variables.size() > kVerifiedArithmeticMaxVariables) {
        return fail("arithmetic_variable_count_invalid");
    }
    if (ir.operations.empty() ||
        ir.operations.size() > kVerifiedArithmeticMaxOperations) {
        return fail("arithmetic_operation_count_invalid");
    }
    if (ir.max_chunk_size == 0 ||
        ir.max_chunk_size > kVerifiedArithmeticMaxChunkSize) {
        return fail("arithmetic_max_chunk_size_invalid");
    }
    for (const auto& [name, value] : ir.initial_variables) {
        if (!verified_execution_detail::arithmetic_identifier_valid(name) ||
            !verified_execution_detail::arithmetic_value_in_range(value)) {
            return fail("arithmetic_initial_variable_invalid:" + name);
        }
    }
    if (ir.checksum.modulus <= 0 ||
        ir.checksum.modulus > kVerifiedArithmeticLiteralLimit ||
        ir.checksum.weights.size() != ir.initial_variables.size()) {
        return fail("arithmetic_checksum_contract_shape_invalid");
    }
    for (const auto& [name, weight] : ir.checksum.weights) {
        if (ir.initial_variables.count(name) == 0 ||
            weight < -kVerifiedArithmeticLiteralLimit ||
            weight > kVerifiedArithmeticLiteralLimit) {
            return fail("arithmetic_checksum_weight_invalid:" + name);
        }
    }
    const auto known = [&](const std::string& name) {
        return !name.empty() && ir.initial_variables.count(name) != 0;
    };
    for (std::size_t index = 0; index < ir.operations.size(); ++index) {
        const auto& operation = ir.operations[index];
        if (operation.index != index + 1) {
            return fail("arithmetic_ir_operation_index_invalid:" +
                        std::to_string(index + 1));
        }
        switch (operation.kind) {
            case SmartThinkingArithmeticOperationKind::AffineMod:
                if (!known(operation.target) ||
                    !verified_execution_detail::arithmetic_literal_in_range(
                        operation.multiplier) ||
                    !verified_execution_detail::arithmetic_literal_in_range(
                        operation.addend) || operation.modulus <= 0 ||
                    operation.modulus > kVerifiedArithmeticLiteralLimit) {
                    return fail("arithmetic_ir_affine_invalid:" +
                                std::to_string(index + 1));
                }
                break;
            case SmartThinkingArithmeticOperationKind::ParityAdjust:
                if (!known(operation.condition) || !known(operation.target)) {
                    return fail("arithmetic_ir_parity_invalid:" +
                                std::to_string(index + 1));
                }
                break;
            case SmartThinkingArithmeticOperationKind::ProductAccumulateMod:
                if (!known(operation.target) || !known(operation.lhs) ||
                    !known(operation.rhs) ||
                    !verified_execution_detail::arithmetic_literal_in_range(
                        operation.addend) || operation.modulus <= 0 ||
                    operation.modulus > kVerifiedArithmeticLiteralLimit) {
                    return fail("arithmetic_ir_product_invalid:" +
                                std::to_string(index + 1));
                }
                break;
            case SmartThinkingArithmeticOperationKind::ConditionalPairReplace:
                if (!known(operation.condition) || !known(operation.left) ||
                    !known(operation.right) || operation.left == operation.right ||
                    !verified_execution_detail::arithmetic_literal_in_range(
                        operation.threshold)) {
                    return fail("arithmetic_ir_pair_invalid:" +
                                std::to_string(index + 1));
                }
                break;
            case SmartThinkingArithmeticOperationKind::DivisibleAdd:
                if (!known(operation.condition) || !known(operation.target) ||
                    operation.divisor <= 0 ||
                    operation.divisor > kVerifiedArithmeticLiteralLimit ||
                    !verified_execution_detail::arithmetic_literal_in_range(
                        operation.else_addend)) {
                    return fail("arithmetic_ir_divisible_invalid:" +
                                std::to_string(index + 1));
                }
                break;
            default:
                return fail("arithmetic_operation_kind_invalid:" +
                            std::to_string(index + 1));
        }
    }
    return true;
}

inline SmartThinkingArithmeticExecutionResult execute_smart_thinking_arithmetic_ir(
    const SmartThinkingArithmeticIR& ir,
    std::int64_t deadline_ms = 250) {
    SmartThinkingArithmeticExecutionResult result;
    std::string validation_failure;
    if (!validate_smart_thinking_arithmetic_ir(ir, &validation_failure)) {
        result.failure_reason = validation_failure;
        return result;
    }
    const auto started = std::chrono::steady_clock::now();
    result.variables = ir.initial_variables;
    for (std::size_t index = 0; index < ir.operations.size(); ++index) {
        if (deadline_ms >= 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count() > deadline_ms) {
            result.failure_reason = "arithmetic_execution_deadline_exceeded";
            return result;
        }
        const auto& operation = ir.operations[index];
        const auto value = [&](const std::string& name) {
            return result.variables.at(name);
        };
        std::int64_t updated = 0;
        switch (operation.kind) {
            case SmartThinkingArithmeticOperationKind::AffineMod:
                if (!verified_execution_detail::modular_product_sum(
                        0, value(operation.target), operation.multiplier,
                        operation.addend, operation.modulus, &updated)) {
                    result.failure_reason = "arithmetic_modular_evaluation_failed";
                    return result;
                }
                result.variables[operation.target] = updated;
                break;
            case SmartThinkingArithmeticOperationKind::ParityAdjust:
                if (value(operation.condition) % 2 == 0) {
                    if (!verified_execution_detail::checked_add(
                            value(operation.target), value(operation.condition),
                            &updated)) {
                        result.failure_reason = "arithmetic_overflow";
                        return result;
                    }
                } else if (!verified_execution_detail::checked_sub(
                               value(operation.target), value(operation.condition),
                               &updated)) {
                    result.failure_reason = "arithmetic_overflow";
                    return result;
                }
                result.variables[operation.target] = updated;
                break;
            case SmartThinkingArithmeticOperationKind::ProductAccumulateMod:
                if (!verified_execution_detail::modular_product_sum(
                        value(operation.target), value(operation.lhs),
                        value(operation.rhs), operation.addend,
                        operation.modulus, &updated)) {
                    result.failure_reason = "arithmetic_modular_evaluation_failed";
                    return result;
                }
                result.variables[operation.target] = updated;
                break;
            case SmartThinkingArithmeticOperationKind::ConditionalPairReplace: {
                const auto left = value(operation.left);
                const auto right = value(operation.right);
                if (value(operation.condition) >= operation.threshold) {
                    if (!verified_execution_detail::checked_sub(right, left,
                                                                &updated)) {
                        result.failure_reason = "arithmetic_overflow";
                        return result;
                    }
                    result.variables[operation.left] = updated;
                    result.variables[operation.right] = left;
                } else {
                    if (!verified_execution_detail::checked_add(left, right,
                                                                &updated)) {
                        result.failure_reason = "arithmetic_overflow";
                        return result;
                    }
                    result.variables[operation.left] = updated;
                    result.variables[operation.right] = right;
                }
                break;
            }
            case SmartThinkingArithmeticOperationKind::DivisibleAdd: {
                const auto condition = value(operation.condition);
                const auto increment = condition % operation.divisor == 0
                    ? condition / operation.divisor : operation.else_addend;
                if (!verified_execution_detail::checked_add(
                        value(operation.target), increment, &updated)) {
                    result.failure_reason = "arithmetic_overflow";
                    return result;
                }
                result.variables[operation.target] = updated;
                break;
            }
            default:
                result.failure_reason = "arithmetic_operation_kind_invalid";
                return result;
        }
        for (const auto& [name, current] : result.variables) {
            (void)name;
            if (!verified_execution_detail::arithmetic_value_in_range(current)) {
                result.failure_reason = "arithmetic_state_magnitude_exceeded";
                return result;
            }
        }
        ++result.operations_executed;
    }
    result.chunks_executed =
        (result.operations_executed + ir.max_chunk_size - 1) / ir.max_chunk_size;
    std::int64_t checksum = 0;
    for (const auto& [name, weight] : ir.checksum.weights) {
        if (!verified_execution_detail::modular_product_sum(
                checksum, result.variables.at(name), weight, 0,
                ir.checksum.modulus, &checksum)) {
            result.failure_reason = "arithmetic_checksum_failed";
            return result;
        }
    }
    result.checksum = checksum;
    result.completed = true;
    return result;
}

inline bool validate_smart_thinking_dispatch_ir(
    const SmartThinkingDispatchIR& ir,
    std::string* failure_reason = nullptr) {
    const auto fail = [&](const std::string& reason) {
        if (failure_reason != nullptr) *failure_reason = reason;
        return false;
    };
    if (ir.family_id != "dispatch_event_program" ||
        ir.contract_version != "1") {
        return fail("dispatch_ir_contract_invalid");
    }
    if (ir.worker_count < 1 ||
        ir.worker_count > static_cast<int>(kVerifiedDispatchMaxWorkers)) {
        return fail("dispatch_worker_count_invalid");
    }
    if (ir.tasks.empty() || ir.tasks.size() > kVerifiedDispatchMaxTasks ||
        ir.priority.size() != ir.tasks.size()) {
        return fail("dispatch_task_set_size_invalid");
    }
    if (ir.max_chunk_size == 0 ||
        ir.max_chunk_size > kVerifiedDispatchMaxChunkSize) {
        return fail("dispatch_max_chunk_size_invalid");
    }
    std::set<std::string> priority_seen;
    for (const auto& id : ir.priority) {
        if (ir.tasks.count(id) == 0 || !priority_seen.insert(id).second) {
            return fail("dispatch_priority_not_permutation");
        }
    }
    std::set<std::int64_t> indices;
    std::size_t edge_count = 0;
    std::map<std::string, int> indegree;
    std::map<std::string, std::vector<std::string>> outgoing;
    for (const auto& [id, task] : ir.tasks) {
        if (id != task.id ||
            !verified_execution_detail::dispatch_identifier_valid(id) ||
            task.duration <= 0 ||
            task.duration > kVerifiedDispatchMaxDuration ||
            task.checksum_index < 1 ||
            task.checksum_index > static_cast<std::int64_t>(ir.tasks.size()) ||
            !indices.insert(task.checksum_index).second) {
            return fail("dispatch_task_definition_invalid:" + id);
        }
        indegree[id] = 0;
    }
    for (const auto& [id, task] : ir.tasks) {
        std::set<std::string> seen;
        for (const auto& dependency : task.dependencies) {
            if (++edge_count > kVerifiedDispatchMaxDependencies) {
                return fail("dispatch_dependency_limit_exceeded");
            }
            if (ir.tasks.count(dependency) == 0 || dependency == id ||
                !seen.insert(dependency).second) {
                return fail("dispatch_dependency_invalid:" + id);
            }
            ++indegree[id];
            outgoing[dependency].push_back(id);
        }
    }
    std::vector<std::string> ready;
    for (const auto& [id, degree] : indegree) {
        if (degree == 0) ready.push_back(id);
    }
    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto id = ready.back();
        ready.pop_back();
        ++visited;
        for (const auto& next : outgoing[id]) {
            if (--indegree[next] == 0) ready.push_back(next);
        }
    }
    if (visited != ir.tasks.size()) return fail("dispatch_dependency_cycle");
    if (!ir.policy.non_preemptive ||
        ir.policy.ready_order != SmartThinkingDispatchReadyOrder::PriorityThenId ||
        ir.policy.worker_order !=
            SmartThinkingDispatchWorkerOrder::AscendingWorkerNumber ||
        ir.policy.completion_tie_break !=
            SmartThinkingDispatchCompletionTieBreak::WorkerNumberBeforeRedispatch ||
        ir.checksum.worker_weight <= 0 ||
        ir.checksum.worker_weight > kVerifiedDispatchMaxChecksumLiteral ||
        ir.checksum.start_weight <= 0 ||
        ir.checksum.start_weight > kVerifiedDispatchMaxChecksumLiteral ||
        ir.checksum.finish_weight <= 0 ||
        ir.checksum.finish_weight > kVerifiedDispatchMaxChecksumLiteral ||
        ir.checksum.modulus <= 0 ||
        ir.checksum.modulus > kVerifiedDispatchMaxChecksumLiteral) {
        return fail("dispatch_policy_or_checksum_invalid");
    }
    return true;
}

inline SmartThinkingDispatchExecutionResult execute_smart_thinking_dispatch_ir(
    const SmartThinkingDispatchIR& ir,
    std::int64_t deadline_ms = 500) {
    SmartThinkingDispatchExecutionResult result;
    std::string validation_failure;
    if (!validate_smart_thinking_dispatch_ir(ir, &validation_failure)) {
        result.failure_reason = validation_failure;
        return result;
    }
    const auto started = std::chrono::steady_clock::now();
    std::map<std::string, std::size_t> priority_index;
    for (std::size_t index = 0; index < ir.priority.size(); ++index) {
        priority_index[ir.priority[index]] = index;
    }
    std::set<std::string> remaining;
    for (const auto& [id, task] : ir.tasks) {
        (void)task;
        remaining.insert(id);
    }
    std::set<std::string> completed;
    std::map<int, std::pair<std::string, std::int64_t>> running;
    std::int64_t time = 0;

    const auto ready_tasks = [&]() {
        std::vector<std::string> ready;
        for (const auto& id : remaining) {
            bool dependencies_complete = true;
            for (const auto& dependency : ir.tasks.at(id).dependencies) {
                if (completed.count(dependency) == 0) {
                    dependencies_complete = false;
                    break;
                }
            }
            if (dependencies_complete) ready.push_back(id);
        }
        std::sort(ready.begin(), ready.end(), [&](const auto& lhs, const auto& rhs) {
            const auto left = priority_index.at(lhs);
            const auto right = priority_index.at(rhs);
            return left != right ? left < right : lhs < rhs;
        });
        return ready;
    };

    while (!remaining.empty() || !running.empty()) {
        if (deadline_ms >= 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count() > deadline_ms) {
            result.failure_reason = "dispatch_execution_deadline_exceeded";
            return result;
        }
        std::vector<int> free_workers;
        for (int worker = 1; worker <= ir.worker_count; ++worker) {
            if (running.count(worker) == 0) free_workers.push_back(worker);
        }
        const auto ready = ready_tasks();
        const auto assign_count = std::min(free_workers.size(), ready.size());
        for (std::size_t index = 0; index < assign_count; ++index) {
            const int worker = free_workers[index];
            const auto& id = ready[index];
            std::int64_t finish = 0;
            if (!verified_execution_detail::checked_add(
                    time, ir.tasks.at(id).duration, &finish)) {
                result.failure_reason = "dispatch_time_overflow";
                return result;
            }
            running[worker] = {id, finish};
            result.schedule[id] = {worker, time, finish};
            remaining.erase(id);
        }
        if (running.empty()) {
            result.failure_reason = "dispatch_deadlock";
            return result;
        }
        std::int64_t next_time = std::numeric_limits<std::int64_t>::max();
        for (const auto& [worker, task] : running) {
            (void)worker;
            next_time = std::min(next_time, task.second);
        }
        time = next_time;
        for (int worker = 1; worker <= ir.worker_count; ++worker) {
            const auto found = running.find(worker);
            if (found != running.end() && found->second.second == next_time) {
                completed.insert(found->second.first);
                result.completion_order.push_back(found->second.first);
                running.erase(found);
                ++result.tasks_completed;
            }
        }
        ++result.completion_events;
    }
    result.makespan = time;
    std::int64_t total = 0;
    for (const auto& [id, task] : ir.tasks) {
        const auto& entry = result.schedule.at(id);
        std::int64_t weighted = 0;
        if (!verified_execution_detail::modular_product_sum(
                0, entry.worker, ir.checksum.worker_weight, 0,
                ir.checksum.modulus, &weighted) ||
            !verified_execution_detail::modular_product_sum(
                weighted, entry.start, ir.checksum.start_weight, 0,
                ir.checksum.modulus, &weighted) ||
            !verified_execution_detail::modular_product_sum(
                weighted, entry.finish, ir.checksum.finish_weight, 0,
                ir.checksum.modulus, &weighted) ||
            !verified_execution_detail::modular_product_sum(
                total, task.checksum_index, weighted, 0,
                ir.checksum.modulus, &total)) {
            result.failure_reason = "dispatch_checksum_failed";
            return result;
        }
    }
    result.checksum = total;
    result.completed = result.tasks_completed == ir.tasks.size();
    if (!result.completed) result.failure_reason = "dispatch_incomplete";
    return result;
}


inline constexpr std::size_t kVerifiedSelectionMaxItems = 48;
inline constexpr std::size_t kVerifiedSelectionMaxCount = 10;
inline constexpr std::size_t kVerifiedSelectionMaxRequiredTags = 16;
inline constexpr std::size_t kVerifiedSelectionMaxForbiddenPairs = 128;
inline constexpr std::size_t kVerifiedSelectionMaxIdentifierBytes = 64;
inline constexpr std::int64_t kVerifiedSelectionMaxNumericMagnitude =
    1000000000LL;
inline constexpr std::size_t kVerifiedSelectionMaxSearchNodes = 5000000;

namespace verified_selection_detail {

inline bool identifier_valid(const std::string& value) {
    if (value.empty() || value.size() > kVerifiedSelectionMaxIdentifierBytes) {
        return false;
    }
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::iscntrl(c) || std::isspace(c)) return false;
    }
    return true;
}

inline bool tag_valid(const std::string& value) {
    if (value.empty() || value.size() > kVerifiedSelectionMaxIdentifierBytes) {
        return false;
    }
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (!(std::isalnum(c) || raw == '_' || raw == '-')) return false;
    }
    return true;
}

inline bool checked_add(std::int64_t lhs,
                        std::int64_t rhs,
                        std::int64_t* result) {
    return verified_execution_detail::checked_add(lhs, rhs, result);
}

}  // namespace verified_selection_detail

inline bool validate_smart_thinking_constrained_selection_ir(
    const SmartThinkingConstrainedSelectionIR& ir,
    std::string* failure_reason = nullptr) {
    const auto fail = [&](const std::string& reason) {
        if (failure_reason != nullptr) *failure_reason = reason;
        return false;
    };
    if (ir.family_id != "constrained_selection" ||
        ir.contract_version != "1") {
        return fail("selection_ir_contract_invalid");
    }
    if (ir.items.empty() || ir.items.size() > kVerifiedSelectionMaxItems ||
        ir.min_count == 0 || ir.min_count > ir.max_count ||
        ir.max_count > kVerifiedSelectionMaxCount ||
        ir.max_count > ir.items.size()) {
        return fail("selection_count_limits_invalid");
    }
    if (ir.budget < 0 || ir.risk_cap < 0 ||
        ir.budget > kVerifiedSelectionMaxNumericMagnitude ||
        ir.risk_cap > kVerifiedSelectionMaxNumericMagnitude) {
        return fail("selection_resource_limits_invalid");
    }
    if (ir.required_tags.size() > kVerifiedSelectionMaxRequiredTags ||
        ir.forbidden_pairs.size() > kVerifiedSelectionMaxForbiddenPairs ||
        ir.max_search_nodes == 0 ||
        ir.max_search_nodes > kVerifiedSelectionMaxSearchNodes) {
        return fail("selection_contract_limits_invalid");
    }
    if (ir.objective !=
            SmartThinkingConstrainedSelectionObjective::MaximizeTotalValue ||
        ir.tie_break != SmartThinkingConstrainedSelectionTieBreak::
            LowerTotalCostThenLexicographicIds) {
        return fail("selection_policy_invalid");
    }

    std::set<std::string> all_tags;
    for (const auto& [id, item] : ir.items) {
        if (id != item.id ||
            !verified_selection_detail::identifier_valid(id) ||
            item.cost < 0 || item.risk < 0 ||
            item.cost > kVerifiedSelectionMaxNumericMagnitude ||
            item.risk > kVerifiedSelectionMaxNumericMagnitude ||
            item.value < -kVerifiedSelectionMaxNumericMagnitude ||
            item.value > kVerifiedSelectionMaxNumericMagnitude ||
            item.tags.size() > kVerifiedSelectionMaxRequiredTags) {
            return fail("selection_item_invalid:" + id);
        }
        std::set<std::string> item_tags;
        for (const auto& tag : item.tags) {
            if (!verified_selection_detail::tag_valid(tag) ||
                !item_tags.insert(tag).second) {
                return fail("selection_item_tag_invalid:" + id);
            }
            all_tags.insert(tag);
        }
    }

    std::set<std::string> required_tags;
    for (const auto& tag : ir.required_tags) {
        if (!verified_selection_detail::tag_valid(tag) ||
            !required_tags.insert(tag).second) {
            return fail("selection_required_tag_invalid:" + tag);
        }
        if (all_tags.count(tag) == 0) {
            return fail("selection_required_tag_unavailable:" + tag);
        }
    }

    std::set<std::pair<std::string, std::string>> pairs;
    for (const auto& pair : ir.forbidden_pairs) {
        if (pair.first == pair.second || ir.items.count(pair.first) == 0 ||
            ir.items.count(pair.second) == 0) {
            return fail("selection_forbidden_pair_invalid");
        }
        const auto canonical = pair.first < pair.second
            ? std::make_pair(pair.first, pair.second)
            : std::make_pair(pair.second, pair.first);
        if (!pairs.insert(canonical).second) {
            return fail("selection_forbidden_pair_duplicate");
        }
    }
    return true;
}

struct SmartThinkingConstrainedSelectionExecutionResult {
    bool completed = false;
    std::vector<std::string> selected;
    std::int64_t total_cost = 0;
    std::int64_t total_risk = 0;
    std::int64_t total_value = 0;
    std::size_t search_nodes = 0;
    std::string failure_reason;
};

inline SmartThinkingConstrainedSelectionExecutionResult
execute_smart_thinking_constrained_selection_ir(
    const SmartThinkingConstrainedSelectionIR& ir,
    std::int64_t deadline_ms = -1) {
    SmartThinkingConstrainedSelectionExecutionResult result;
    std::string validation_failure;
    if (!validate_smart_thinking_constrained_selection_ir(
            ir, &validation_failure)) {
        result.failure_reason = validation_failure;
        return result;
    }

    const auto started = std::chrono::steady_clock::now();
    std::vector<const SmartThinkingConstrainedSelectionItemIR*> items;
    items.reserve(ir.items.size());
    for (const auto& [id, item] : ir.items) {
        (void)id;
        items.push_back(&item);
    }
    const std::size_t item_count = items.size();

    std::map<std::string, std::size_t> required_tag_index;
    for (std::size_t index = 0; index < ir.required_tags.size(); ++index) {
        required_tag_index[ir.required_tags[index]] = index;
    }
    const std::uint64_t all_required_mask = ir.required_tags.empty()
        ? 0
        : ((std::uint64_t{1} << ir.required_tags.size()) - 1);
    std::vector<std::uint64_t> item_tag_masks(item_count, 0);
    for (std::size_t index = 0; index < item_count; ++index) {
        for (const auto& tag : items[index]->tags) {
            const auto found = required_tag_index.find(tag);
            if (found != required_tag_index.end()) {
                item_tag_masks[index] |= std::uint64_t{1} << found->second;
            }
        }
    }

    std::map<std::string, std::size_t> item_index;
    for (std::size_t index = 0; index < item_count; ++index) {
        item_index[items[index]->id] = index;
    }
    std::vector<std::vector<bool>> forbidden(
        item_count, std::vector<bool>(item_count, false));
    for (const auto& pair : ir.forbidden_pairs) {
        const auto left = item_index.at(pair.first);
        const auto right = item_index.at(pair.second);
        forbidden[left][right] = true;
        forbidden[right][left] = true;
    }

    std::vector<std::vector<std::int64_t>> optimistic(
        item_count + 1,
        std::vector<std::int64_t>(ir.max_count + 1, 0));
    for (std::size_t offset = 0; offset < item_count; ++offset) {
        const std::size_t index = item_count - 1 - offset;
        optimistic[index][0] = 0;
        for (std::size_t slots = 1; slots <= ir.max_count; ++slots) {
            optimistic[index][slots] = optimistic[index + 1][slots];
            if (items[index]->value > 0) {
                std::int64_t with_item = 0;
                if (!verified_selection_detail::checked_add(
                        items[index]->value,
                        optimistic[index + 1][slots - 1], &with_item)) {
                    result.failure_reason = "selection_value_overflow";
                    return result;
                }
                optimistic[index][slots] =
                    std::max(optimistic[index][slots], with_item);
            }
        }
    }

    bool found_solution = false;
    bool resource_exhausted = false;
    std::vector<std::size_t> selected_indices;
    std::vector<std::size_t> best_indices;
    std::int64_t best_cost = 0;
    std::int64_t best_risk = 0;
    std::int64_t best_value = 0;

    const auto candidate_ids = [&](const std::vector<std::size_t>& indices) {
        std::vector<std::string> ids;
        ids.reserve(indices.size());
        for (const auto index : indices) ids.push_back(items[index]->id);
        return ids;
    };

    const auto better_candidate = [&](std::int64_t value,
                                      std::int64_t cost,
                                      const std::vector<std::size_t>& indices) {
        if (!found_solution || value != best_value) {
            return !found_solution || value > best_value;
        }
        if (cost != best_cost) return cost < best_cost;
        return candidate_ids(indices) < candidate_ids(best_indices);
    };

    std::function<void(std::size_t, std::int64_t, std::int64_t,
                       std::int64_t, std::uint64_t)> search;
    search = [&](std::size_t index,
                 std::int64_t cost,
                 std::int64_t risk,
                 std::int64_t value,
                 std::uint64_t tag_mask) {
        if (resource_exhausted) return;
        ++result.search_nodes;
        if (result.search_nodes > ir.max_search_nodes) {
            resource_exhausted = true;
            result.failure_reason = "selection_search_node_limit_exceeded";
            return;
        }
        if ((result.search_nodes & 1023U) == 0U && deadline_ms >= 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count() >
                deadline_ms) {
            resource_exhausted = true;
            result.failure_reason = "selection_execution_deadline_exceeded";
            return;
        }
        const std::size_t selected_count = selected_indices.size();
        if (selected_count > ir.max_count || cost > ir.budget ||
            risk > ir.risk_cap) {
            return;
        }
        if (selected_count + (item_count - index) < ir.min_count) return;

        if (selected_count >= ir.min_count &&
            (tag_mask & all_required_mask) == all_required_mask &&
            better_candidate(value, cost, selected_indices)) {
            found_solution = true;
            best_indices = selected_indices;
            best_cost = cost;
            best_risk = risk;
            best_value = value;
        }
        if (index >= item_count || selected_count >= ir.max_count) return;

        const std::size_t slots = ir.max_count - selected_count;
        std::int64_t upper_value = 0;
        if (!verified_selection_detail::checked_add(
                value, optimistic[index][slots], &upper_value)) {
            resource_exhausted = true;
            result.failure_reason = "selection_value_overflow";
            return;
        }
        if (found_solution && upper_value < best_value) return;

        bool pair_conflict = false;
        for (const auto selected : selected_indices) {
            if (forbidden[index][selected]) {
                pair_conflict = true;
                break;
            }
        }
        if (!pair_conflict) {
            std::int64_t next_cost = 0;
            std::int64_t next_risk = 0;
            std::int64_t next_value = 0;
            if (!verified_selection_detail::checked_add(
                    cost, items[index]->cost, &next_cost) ||
                !verified_selection_detail::checked_add(
                    risk, items[index]->risk, &next_risk) ||
                !verified_selection_detail::checked_add(
                    value, items[index]->value, &next_value)) {
                resource_exhausted = true;
                result.failure_reason = "selection_total_overflow";
                return;
            }
            selected_indices.push_back(index);
            search(index + 1, next_cost, next_risk, next_value,
                   tag_mask | item_tag_masks[index]);
            selected_indices.pop_back();
        }
        search(index + 1, cost, risk, value, tag_mask);
    };

    search(0, 0, 0, 0, 0);
    if (resource_exhausted) return result;
    if (!found_solution) {
        result.failure_reason = "selection_no_feasible_solution";
        return result;
    }
    result.selected = candidate_ids(best_indices);
    result.total_cost = best_cost;
    result.total_risk = best_risk;
    result.total_value = best_value;
    result.completed = true;
    return result;
}

}  // namespace lemon
