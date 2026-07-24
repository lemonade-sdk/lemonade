#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lemon {

enum class SmartThinkingArithmeticOperationKind {
    AffineMod,
    ParityAdjust,
    ProductAccumulateMod,
    ConditionalPairReplace,
    DivisibleAdd
};

struct SmartThinkingArithmeticOperationIR {
    SmartThinkingArithmeticOperationKind kind =
        SmartThinkingArithmeticOperationKind::AffineMod;
    std::size_t index = 0;
    std::string target;
    std::string condition;
    std::string lhs;
    std::string rhs;
    std::string left;
    std::string right;
    std::int64_t multiplier = 0;
    std::int64_t addend = 0;
    std::int64_t modulus = 0;
    std::int64_t threshold = 0;
    std::int64_t divisor = 0;
    std::int64_t else_addend = 0;
};

struct SmartThinkingArithmeticChecksumIR {
    std::map<std::string, std::int64_t> weights;
    std::int64_t modulus = 0;
};

struct SmartThinkingArithmeticIR {
    std::string family_id = "arithmetic_state_program";
    std::string contract_version = "1";
    std::map<std::string, std::int64_t> initial_variables;
    std::vector<SmartThinkingArithmeticOperationIR> operations;
    SmartThinkingArithmeticChecksumIR checksum;
    std::size_t max_chunk_size = 8;
    std::string program_hash;
};

enum class SmartThinkingDispatchReadyOrder {
    PriorityThenId
};

enum class SmartThinkingDispatchWorkerOrder {
    AscendingWorkerNumber
};

enum class SmartThinkingDispatchCompletionTieBreak {
    WorkerNumberBeforeRedispatch
};

struct SmartThinkingDispatchTaskIR {
    std::string id;
    std::int64_t duration = 0;
    std::vector<std::string> dependencies;
    std::int64_t checksum_index = 0;
};

struct SmartThinkingDispatchPolicyIR {
    SmartThinkingDispatchReadyOrder ready_order =
        SmartThinkingDispatchReadyOrder::PriorityThenId;
    SmartThinkingDispatchWorkerOrder worker_order =
        SmartThinkingDispatchWorkerOrder::AscendingWorkerNumber;
    SmartThinkingDispatchCompletionTieBreak completion_tie_break =
        SmartThinkingDispatchCompletionTieBreak::WorkerNumberBeforeRedispatch;
    bool non_preemptive = true;
};

struct SmartThinkingDispatchChecksumIR {
    std::int64_t worker_weight = 101;
    std::int64_t start_weight = 17;
    std::int64_t finish_weight = 29;
    std::int64_t modulus = 1000003;
};

struct SmartThinkingDispatchIR {
    std::string family_id = "dispatch_event_program";
    std::string contract_version = "1";
    int worker_count = 0;
    std::map<std::string, SmartThinkingDispatchTaskIR> tasks;
    std::vector<std::string> priority;
    SmartThinkingDispatchPolicyIR policy;
    SmartThinkingDispatchChecksumIR checksum;
    std::size_t max_chunk_size = 4;
    std::string program_hash;
};


enum class SmartThinkingConstrainedSelectionObjective {
    MaximizeTotalValue
};

enum class SmartThinkingConstrainedSelectionTieBreak {
    LowerTotalCostThenLexicographicIds
};

struct SmartThinkingConstrainedSelectionItemIR {
    std::string id;
    std::int64_t cost = 0;
    std::int64_t risk = 0;
    std::int64_t value = 0;
    std::vector<std::string> tags;
};

struct SmartThinkingConstrainedSelectionForbiddenPairIR {
    std::string first;
    std::string second;
};

struct SmartThinkingConstrainedSelectionIR {
    std::string family_id = "constrained_selection";
    std::string contract_version = "1";
    std::size_t min_count = 1;
    std::size_t max_count = 1;
    std::int64_t budget = 0;
    std::int64_t risk_cap = 0;
    std::map<std::string, SmartThinkingConstrainedSelectionItemIR> items;
    // Every required tag must be represented by at least one selected item.
    std::vector<std::string> required_tags;
    std::vector<SmartThinkingConstrainedSelectionForbiddenPairIR> forbidden_pairs;
    SmartThinkingConstrainedSelectionObjective objective =
        SmartThinkingConstrainedSelectionObjective::MaximizeTotalValue;
    SmartThinkingConstrainedSelectionTieBreak tie_break =
        SmartThinkingConstrainedSelectionTieBreak::LowerTotalCostThenLexicographicIds;
    std::size_t max_search_nodes = 2000000;
    std::string program_hash;
};

}  // namespace lemon
