#pragma once

#include "backend_definition_runner_internal.h"
#include "lemon/utils/path_utils.h"

#include <map>
#include <sstream>

namespace lemon::backends {

inline bool hgn_suffix(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::optional<std::vector<std::string>> DefinitionRunner::select_checkpoint_files(
    const std::string& variant, const std::vector<std::string>& files) const {
    if (!halogen_contract() || !hgn_suffix(variant, ".hgn")) return std::nullopt;
    std::vector<std::string> selected;
    for (const auto& file : files) {
        if (file == variant || hgn_suffix(file, "-ngram.hgn") || hgn_suffix(file, "-vision.hgn") ||
            file.rfind("tokenizer/", 0) == 0) selected.push_back(file);
    }
    if (std::find(selected.begin(), selected.end(), variant) == selected.end())
        throw std::runtime_error("Halogen checkpoint not found in repository: " + variant);
    return selected;
}

inline std::optional<utils::SetupFailure> halogen_kfd_failure(const std::string& properties) {
    if (properties.size() > 65536) throw std::runtime_error("Container GPU properties exceed limit");
    std::map<std::string, unsigned long long> values;
    std::istringstream lines(properties);
    std::string key;
    unsigned long long value = 0;
    while (lines >> key >> value) values[key] = value;
    if ((values["capability"] & 0x08000000ULL) == 0) {
        return utils::SetupFailure{"kernel_svm", "capability & 0x08000000 is 0",
            {"Install Linux 6.18.4 or newer, built with CONFIG_HSA_AMD_SVM"}, ""};
    }
    const bool reported = values.count("cwsr_size") && values.count("ctl_stack_size") && values.count("simd_count");
    if (!reported || values["cwsr_size"] != values["ctl_stack_size"] + (values["simd_count"] / 2) * 479232ULL) {
        return utils::SetupFailure{"kernel_cwsr", "cwsr_size is not ctl_stack_size + (simd_count / 2) * 479232",
            {"Install Linux 6.18.4 or newer"}, ""};
    }
    return std::nullopt;
}

inline void check_halogen_host(const utils::ContainerRunSpec& spec, const LaunchServices& services) {
    auto nodes = services.container_gpu_nodes();
    if (nodes.size() > 256) throw std::runtime_error("Container GPU topology exceeds node limit");
    std::sort(nodes.begin(), nodes.end(), [](const auto& left, const auto& right) { return left.node < right.node; });
    for (const auto& node : nodes) {
        if (!services.container_supports_arch(spec.recipe, spec.backend, node.architecture)) continue;
        if (node.architecture != "gfx1151") throw std::runtime_error("Halogen requires a gfx1151 KFD node");
        const auto properties = services.container_gpu_properties(node.node);
        if (properties.empty()) return;
        if (const auto failure = halogen_kfd_failure(properties)) throw std::runtime_error(utils::SetupReport{{*failure}}.text());
        return;
    }
}

inline std::map<std::string, std::string> halogen_bundle(const std::string& name, const ModelInfo& model, const LaunchServices& services) {
    const auto checkpoint = model.resolved_path("main");
    if (checkpoint.empty() || !services.exists(checkpoint))
        throw std::runtime_error("halogen: HGN checkpoint not found for model '" + name + "' (checkpoint: " + model.checkpoint() + ")");
    const auto table = model.extra<std::string>("halogen_lookup_table", "");
    if (table.empty()) throw std::runtime_error("halogen: model '" + name + "' names no halogen_lookup_table");
    const auto vision = model.extra<std::string>("halogen_vision_tower", "");
    const auto parent = utils::path_from_utf8(checkpoint).parent_path();
    const auto beside = [&](const std::string& extra) {
        if (extra.empty()) return std::string{};
        const auto path = utils::path_from_utf8(extra).lexically_normal();
        if (path.is_absolute() || path.empty() || *path.begin() == "..") throw std::invalid_argument("Halogen bundle extra must remain in the snapshot");
        return utils::path_to_utf8(parent / path);
    };
    std::map<std::string, std::string> result{{"main", checkpoint}, {"halogen_lookup_table", beside(table)},
        {"halogen_tokenizer", beside("tokenizer")}, {"halogen_vision_tower", beside(vision)}};
    for (const auto* role : {"halogen_lookup_table", "halogen_tokenizer", "halogen_vision_tower"}) {
        const auto& path = result.at(role);
        if (!path.empty() && !services.exists(path))
            throw std::runtime_error("halogen: the bundle for '" + name + "' is missing " + path);
    }
    return result;
}

}
