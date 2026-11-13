#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>

// The frozen llama.cpp oracle predates decision models. These helpers name the
// exact declaration fields added for them, so every frozen comparison still runs
// against the declaration with only those fields taken out.
namespace llamacpp_decision_additions {

inline nlohmann::json expected() {
    return nlohmann::json::parse(R"json({
        "model_contract": "llamacpp_decision_v1",
        "supported_mode": "classification",
        "mode_capability_requirements": {"classification": ["systemone", "zero-shot"]},
        "arg_variant": "cuda",
        "operation": {"id":"classification.buffered","operation":"classification","delivery":"buffered","model_modes":["classification"],
            "transport":{"kind":"http_json","method":"POST","path":"/v1/systemone","port":"http","timeout":"global","response_codec":"json.v1"},
            "request":{"base":"identity"}},
        "argv": [
            {"id":"pooling","flag":"--pooling","value":"none","when":{"ref":"/launch/hook/rule_head"},"reserve":"when_active"},
            {"id":"batch","flag":"-b","value":{"string":{"ref":"/launch/hook/batch_size"}},"when":{"ref":"/launch/hook/add_batch_size"},"aliases":["--batch-size"],"reserve":"when_active"},
            {"id":"ubatch","flag":"-ub","value":{"string":{"ref":"/launch/hook/batch_size"}},"when":{"ref":"/launch/hook/add_ubatch_size"},"aliases":["--ubatch-size"],"reserve":"when_active"}
        ]
    })json");
}

inline void erase_value(nlohmann::json& array, const nlohmann::json& value) {
    array.erase(std::remove(array.begin(), array.end(), value), array.end());
}

inline void erase_id(nlohmann::json& array, const std::string& id) {
    array.erase(std::remove_if(array.begin(), array.end(), [&](const auto& item) { return item.value("id", "") == id; }), array.end());
}

inline const nlohmann::json* find_id(const nlohmann::json& array, const std::string& id) {
    for (const auto& item : array) if (item.value("id", "") == id) return &item;
    return nullptr;
}

inline bool subset(const nlohmann::json& expected_fields, const nlohmann::json& actual) {
    for (const auto& [key, value] : expected_fields.items()) if (!actual.contains(key) || actual.at(key) != value) return false;
    return true;
}

inline bool present(const nlohmann::json& declaration) {
    const auto additions = expected();
    const auto& metadata = declaration.at("metadata");
    const auto& modes = metadata.at("supported_modes");
    const auto& variants = metadata.at("arg_variants");
    const auto* operation = find_id(declaration.at("operations"), "classification.buffered");
    bool argv = true;
    for (const auto& item : additions.at("argv")) {
        const auto* actual = find_id(declaration.at("launch").at("argv"), item.at("id").get<std::string>());
        argv = argv && actual && subset(item, *actual);
    }
    return declaration.value("model_contract", "") == additions.at("model_contract") &&
        std::count(modes.begin(), modes.end(), additions.at("supported_mode")) == 1 &&
        metadata.value("mode_capability_requirements", nlohmann::json()) == additions.at("mode_capability_requirements") &&
        std::count(variants.begin(), variants.end(), additions.at("arg_variant")) == 1 &&
        operation && subset(additions.at("operation"), *operation) && argv;
}

inline nlohmann::json removed(nlohmann::json declaration) {
    const auto additions = expected();
    declaration.erase("model_contract");
    auto& metadata = declaration.at("metadata");
    erase_value(metadata.at("supported_modes"), additions.at("supported_mode"));
    metadata.erase("mode_capability_requirements");
    erase_value(metadata.at("arg_variants"), additions.at("arg_variant"));
    erase_id(declaration.at("operations"), "classification.buffered");
    for (const auto& item : additions.at("argv")) erase_id(declaration.at("launch").at("argv"), item.at("id").get<std::string>());
    return declaration;
}

}
