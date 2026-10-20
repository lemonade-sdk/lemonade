#pragma once

#include "backend_definition_exports_internal.h"

#include <fstream>
#include <sstream>

namespace export_fixture {

using namespace lemon::backends;
using Json = nlohmann::json;

inline std::string read_bytes(const std::string& name) {
    std::ifstream input(std::string(LEMON_TEST_FIXTURES) + "/backend-v2/" + name);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

inline BuiltinExportContract contract() {
    const auto pins = Json::parse(read_bytes("export.synthetic.pins.json"));
    BuiltinExportContract result{{"test-export", 1}, pins.at("source"), pins.at("definition"),
        pins.at("dependencies"), 1, pins.at("policy"), {}};
    for (const auto& record : pins.at("slots")) {
        const auto& slot = record.at("slot");
        ExportSlotPolicy policy{{static_cast<SlotOwnerKind>(slot.at("owner_kind").get<unsigned>()), slot.at("owner_id"), slot.at("stage"), slot.at("hook_id")},
            1, record.at("parameters"), record.at("mutation_policy_digest"),
            static_cast<SlotRemoval>(record.at("removal").get<unsigned>()), {}};
        policy.sealed_inputs.push_back({"/launch/program", {}, ""});
        MutableDataRule mutable_name{{"/metadata/display_name", {}, ""}, MutableValueType::String};
        mutable_name.maximum_length = 128;
        policy.mutable_inputs.push_back(mutable_name);
        policy.effects = {HookEffect::Pure};
        policy.owner_semantics_digest = record.at("owner_semantics");
        policy.ordered_hooks = record.at("ordered_hooks").get<std::vector<std::string>>();
        result.slots.push_back(std::move(policy));
    }
    return result;
}

inline DefinitionSnapshotResult snapshot() {
    const auto bytes = read_bytes("export.synthetic.json");
    return make_v2_source_snapshot({{{DefinitionNamespace::Builtin, "test-export", 1}, bytes, "/private/sentinel.json"}});
}

inline void repin(BuiltinExportContract& value) {
    for (auto& policy : value.slots) policy.mutation_policy_digest = v2::mutation_policy_digest(policy);
    value.policy_digest = v2::export_policy_digest(value);
}

}
