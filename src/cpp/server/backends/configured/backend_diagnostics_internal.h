#pragma once

#include "backend_definition_execution_internal.h"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>

namespace lemon::backends::execution {

inline const std::array<std::string_view, 13>& diagnostic_formats() {
    static const std::array<std::string_view, 13> values = {
        "Loading model: {model:string}",
        "Per-model settings: {settings:options}",
        "Using LlamaCpp Backend: {backend:string}",
        "Using GGUF: {main:path}",
        "Using backend: {backend:string}\n[LlamaCpp] Use GPU: {gpu:boolean}",
        "Skipping mmproj argument since GPU mode is not enabled",
        "Model supports embeddings, adding --embeddings flag",
        "Model supports reranking, adding --reranking flag",
        "Adding custom arguments: {custom:string}",
        "Starting llama-server...",
        "Starting {model:string} on port {port:integer}",
        "Starting ds4-server ({image:string}) for {main:path} on port {port:integer}",
        "Model loaded on port {loaded_port:integer}"};
    return values;
}

inline std::vector<std::pair<std::string, bool>> diagnostic_parts(const std::string& format) {
    static const std::array<std::string_view, 9> facts = {"model:string", "settings:options", "backend:string",
        "main:path", "gpu:boolean", "custom:string", "image:string", "port:integer", "loaded_port:integer"};
    std::vector<std::pair<std::string, bool>> result;
    std::size_t begin = 0;
    while (begin < format.size()) {
        const auto open = format.find('{', begin);
        const auto literal = format.substr(begin, open == std::string::npos ? open : open - begin);
        require(literal.find('}') == std::string::npos, "invalid_diagnostic_fact", "/launch/diagnostics");
        result.emplace_back(literal, false);
        if (open == std::string::npos) break;
        const auto close = format.find('}', open);
        require(close != std::string::npos, "invalid_diagnostic_fact", "/launch/diagnostics");
        const auto fact = format.substr(open + 1, close - open - 1);
        require(std::find(facts.begin(), facts.end(), fact) != facts.end(), "invalid_diagnostic_fact", "/launch/diagnostics");
        result.emplace_back(fact, true);
        begin = close + 1;
    }
    return result;
}

inline Json halogen_starting_diagnostic() {
    return {{"id", "starting"}, {"phase", "starting"}, {"level", "info"}, {"category", "Halogen"},
        {"format", "Starting {model:string} on port {port:integer}"}, {"when", "always"}};
}

inline void compile_diagnostics(Program& program, Budget& budget) {
    const auto& launch = program.normalized.at("launch");
    require(!launch.contains("diagnostics") || !launch.contains("builtin_load_log"), "invalid_diagnostics", "/launch/diagnostics");
    require(!launch.contains("diagnostics") || launch.value("load_log_policy", "none") == "none", "invalid_diagnostics", "/launch/diagnostics");
    auto records = launch.value("diagnostics", Json::array());
    if (launch.contains("builtin_load_log")) records.push_back(halogen_starting_diagnostic());
    require(records.size() <= 32, "limit_exceeded", "/launch/diagnostics");
    const std::vector<std::string> phases = {"loading", "backend", "artifacts", "arguments", "custom_args", "starting", "success", "failure"};
    const std::set<std::string> levels = {"debug", "info", "warning", "error"};
    const std::set<std::string> categories = {"RocmFpx", "LlamaCpp", "Halogen", "DS4"};
    std::set<std::string> ids;
    std::size_t previous = 0;
    for (auto record : records) {
        budget.copy(record);
        const auto phase = record.at("phase").get<std::string>();
        const auto position = std::find(phases.begin(), phases.end(), phase);
        require(position != phases.end(), "invalid_diagnostic_phase", "/launch/diagnostics");
        const auto rank = static_cast<std::size_t>(position - phases.begin());
        require(rank >= previous, "invalid_diagnostic_order", "/launch/diagnostics");
        previous = rank;
        const auto id = record.at("id").get<std::string>();
        require(!id.empty() && id.size() <= 64 && ids.insert(id).second, "invalid_diagnostic_id", "/launch/diagnostics");
        require(levels.count(record.at("level")) && categories.count(record.at("category")) &&
            record.at("category") == program.normalized.at("metadata").at("log_category"), "invalid_diagnostic_category", "/launch/diagnostics");
        const auto format = record.at("format").get<std::string>();
        require(format.size() <= 256 && std::find(diagnostic_formats().begin(), diagnostic_formats().end(), format) != diagnostic_formats().end(),
            "invalid_diagnostic_format", "/launch/diagnostics");
        const auto parts = diagnostic_parts(format);
        for (const auto& [text, fact] : parts) if (fact) {
            const bool backend = text == "backend:string" || text == "gpu:boolean" || text == "image:string";
            require(!backend || rank >= 1, "phase_dependency", "/launch/diagnostics");
            require(text != "main:path" || rank >= 2, "phase_dependency", "/launch/diagnostics");
            require(text != "custom:string" || rank >= 4, "phase_dependency", "/launch/diagnostics");
            require(text != "port:integer" || rank >= 5, "phase_dependency", "/launch/diagnostics");
            require(text != "loaded_port:integer" || phase == "success", "phase_dependency", "/launch/diagnostics");
        }
        if (phase == "backend" || phase == "artifacts") {
            const auto& order = launch.at("prepare_order");
            require(std::find(order.begin(), order.end(), phase) != order.end(), "phase_dependency", "/launch/diagnostics");
        }
        if (phase == "artifacts") {
            const auto& order = launch.at("prepare_order");
            require(std::find(order.begin(), order.end(), "backend") < std::find(order.begin(), order.end(), "artifacts"),
                "phase_dependency", "/launch/diagnostics");
        }
        record["when"] = record.value("when", "always");
        const auto condition = record.at("when").get<std::string>();
        require(condition == "always" || (condition == "main_present" && rank >= 2) ||
            (condition == "custom_present" && rank >= 4) ||
            ((condition == "embedding" || condition == "reranking" || condition == "mmproj_cpu") && rank >= 3),
            "phase_dependency", "/launch/diagnostics");
        budget.keep(sizeof(Json) + format.size() * 4 + id.size() + 256);
        program.launch.diagnostics.push_back(std::move(record));
    }
}

}
