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

inline BuiltinExportContract contract(const std::string& fixture = "export.synthetic", const std::string& id = "test-export") {
    const auto pins = Json::parse(read_bytes(fixture + ".pins.json"));
    BuiltinExportContract result{{id, 1}, pins.at("source"), pins.at("definition"),
        pins.at("dependencies"), pins.at("policy_revision"), pins.at("policy"), {}};
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
    for (const auto& record : pins.at("owners")) result.owners.push_back({record.at("id"), record.at("semantics"),
        record.at("request_hooks").get<std::vector<std::string>>(), record.at("response_hooks").get<std::vector<std::string>>(),
        record.at("handler_hooks").get<std::vector<std::string>>(), record.at("allow_removal"),
        record.at("required_owners").get<std::vector<std::string>>(), record.at("sealed_fields").get<std::vector<std::string>>(),
        record.at("collisions").get<std::vector<std::string>>()});
    result.owner_graph_digest = pins.at("owner_graph_digest");
    return result;
}

inline DefinitionSnapshotResult snapshot(const std::string& fixture = "export.synthetic", const std::string& id = "test-export") {
    const auto bytes = read_bytes(fixture + ".json");
    return make_v2_source_snapshot({{{DefinitionNamespace::Builtin, id, 1}, bytes, "/private/sentinel.json"}});
}

inline void repin(BuiltinExportContract& value) {
    for (auto& policy : value.slots) policy.mutation_policy_digest = v2::mutation_policy_digest(policy);
    value.owner_graph_digest = v2::owner_graph_digest(value.owners);
    value.policy_digest = v2::export_policy_digest(value);
}

}
