#pragma once

#include "lemon/backends/configured/launch_planner.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace lemon::backends {

template<class T> T ds4_memory_fact(const std::optional<T>& value, const char* name) {
    if (!value) throw std::runtime_error(std::string("Missing DS4 memory fact: ") + name);
    return *value;
}

inline json ds4_memory_plan(const ModelInfo& model, const LaunchServices& services) {
    const auto pool_fact = services.integrated_memory_pool();
    const bool available = ds4_memory_fact(pool_fact.available, "integrated_available");
    const auto vram = ds4_memory_fact(pool_fact.vram_gib, "vram_gib");
    const auto window = ds4_memory_fact(pool_fact.virtual_gib, "virtual_gib");
    if (!std::isfinite(vram) || !std::isfinite(window) || vram < 0 || window < 0 ||
        vram > (std::numeric_limits<int>::max)() || window > (std::numeric_limits<int>::max)())
        throw std::invalid_argument("Invalid DS4 memory pool fact");
    const auto pool = available ? (std::max)(vram, window) : 0.0;
    const auto main = ds4_memory_fact(services.artifact_bytes(model.resolved_path("main")), "main_bytes");
    const auto draft = model.resolved_path("draft");
    const auto support = draft.empty() ? ArtifactByteFact{true, 0} : ds4_memory_fact(services.artifact_bytes(draft), "draft_bytes");
    if ((!main.known && main.bytes) || (!support.known && support.bytes)) throw std::invalid_argument("Invalid DS4 artifact byte fact");
    const double weights = static_cast<double>(main.bytes) / 1073741824.0 + static_cast<double>(support.bytes) / 1073741824.0;
    const bool resident = pool > 0.0 && weights > 0.0 && pool >= weights + 16;
    const int expert_cache = resident || pool <= 0.0 ? 0 : static_cast<int>(pool * 0.4);
    const int model_cache = resident || pool <= 0.0 ? 0 : static_cast<int>(pool * 0.2);
    const int reserve = resident ? 16 : pool <= 0.0 ? 0 : static_cast<int>(pool * 0.6);
    const auto working_set = resident ? weights : model.min_resident_gb;
    if (!std::isfinite(working_set) || working_set < 0) throw std::invalid_argument("Invalid DS4 working-set fact");
    json facts{{"integrated_available", available}, {"igpu_vram_gib", vram}, {"igpu_virtual_gib", window},
        {"main_bytes", main.bytes}, {"draft_bytes", support.bytes}, {"pool_gib", pool}, {"weights_gib", weights}, {"working_set_gib", working_set},
        {"main_bytes_known", main.known}, {"draft_bytes_known", support.known},
        {"resident", resident}, {"expert_cache_gb", expert_cache}, {"model_cache_gb", model_cache}, {"free_reserve_gb", reserve}};
    if (working_set > 0.0 && reserve > 0) {
        const auto free_fact = services.free_gpu_memory();
        const bool known = ds4_memory_fact(free_fact.known, "free_known");
        const auto free = ds4_memory_fact(free_fact.gib, "free_gib");
        if (!std::isfinite(free) || free < 0) throw std::invalid_argument("Invalid DS4 free-memory fact");
        facts["free_known"] = known; facts["free_gib"] = free;
        const auto needed = reserve + working_set;
        if (known && free < needed) {
            std::ostringstream message;
            message << std::fixed << std::setprecision(1) << "ds4 needs " << needed
                    << " GiB of free GPU memory to start, and " << free << " GiB is free";
            throw std::runtime_error(message.str());
        }
    }
    return facts;
}

inline json ds4_without_no_think_prefix(json request) {
    const std::string prefix = "/no_think\n";
    if (!request.contains("messages") || !request["messages"].is_array()) return request;
    for (auto& message : request["messages"]) {
        if (message.is_object() && message.value("role", "") == "user" && message.contains("content") && message["content"].is_string()) {
            const auto content = message["content"].get<std::string>();
            if (content.rfind(prefix, 0) == 0) message["content"] = content.substr(prefix.size());
        }
    }
    return request;
}

}
