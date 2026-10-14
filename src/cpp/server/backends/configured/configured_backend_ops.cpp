#include "lemon/backends/configured/configured_backend_ops.h"
#include "lemon/backends/hf_cache_util.h"

#include <algorithm>

namespace lemon::backends {

std::string ConfiguredBackendOps::resolve_checkpoint_path(const ModelInfo& info, const CheckpointResolveContext& context) const {
    const auto artifacts = definition_->records.value("artifacts", json::object());
    const json* selected = nullptr;
    for (const auto& [name, artifact] : artifacts.items()) {
        if (artifact["role"] == "main") selected = &artifact;
    }
    for (const auto& [name, artifact] : artifacts.items()) {
        if (artifact["role"] == context.type) selected = &artifact;
    }
    if (selected && selected->value("cache_resolution", "default") == "active_snapshot") return hf_cache::active_snapshot_or_cache_path(context.model_cache_path);
    return BackendOps::resolve_checkpoint_path(info, context);
}
std::optional<std::vector<std::string>> ConfiguredBackendOps::select_checkpoint_files(const std::string& variant, const std::vector<std::string>& repository_files) const {
    const auto artifacts = definition_->records.value("artifacts", json::object());
    for (const auto& [name, artifact] : artifacts.items()) {
        if (artifact["role"] != "main" || !artifact.contains("files")) continue;
        std::vector<std::string> result;
        for (const auto& file : artifact["files"]) if (std::find(repository_files.begin(), repository_files.end(), file.get<std::string>()) != repository_files.end()) result.push_back(file);
        return result;
    }
    return BackendOps::select_checkpoint_files(variant, repository_files);
}
InstallParams ConfiguredBackendOps::install_params(const InstallContext& context) const {
    if (context.definition.get() != definition_.get() || context.spec.definition.get() != definition_.get()) throw std::invalid_argument("Install definition/spec ownership mismatch");
    const auto& install = definition_->records.at("install");
    if (install["strategy"] != "github_release") throw std::runtime_error("No install params function for recipe: " + context.public_recipe);
    std::string asset_backend = context.backend;
    if (install.value("asset_backend", "identity") == "collapse_rocm_prefix" && asset_backend.rfind("rocm", 0) == 0) asset_backend = "rocm";
    const json values = {{"install", {{"backend", context.backend}, {"asset_backend", asset_backend},
        {"version", context.phase == InstallPhase::Repository ? context.pinned_version : context.resolved_version},
        {"requested_version", context.requested_version}, {"pinned_version", context.pinned_version}, {"resolved_version", context.resolved_version}}},
        {"platform", {{"os", context.platform.os}, {"machine_arch", context.platform.machine_arch}, {"rocm_arch", context.platform.rocm_arch}}}};
    auto matches = [](const json& record, const char* selector, const std::string& value) {
        return !record.contains(selector) || std::find(record[selector].begin(), record[selector].end(), value) != record[selector].end();
    };
    std::vector<size_t> eligible;
    for (size_t i = 0; i < install["cases"].size(); ++i) eligible.push_back(i);
    auto filter = [&](const char* axis, const std::string& value, const std::string& error) {
        eligible.erase(std::remove_if(eligible.begin(), eligible.end(), [&](size_t i) { return !matches(install["cases"][i], axis, value); }), eligible.end());
        if (eligible.empty()) {
            const std::string key = install.contains("errors") && install["errors"].contains(error) ? error : "no_case";
            if (install.contains("errors") && install["errors"].contains(key)) throw std::runtime_error(definition_->required_value("/install/errors/" + key, values).get<std::string>());
            throw std::runtime_error("No matching install case");
        }
    };
    filter("backend", context.backend, "backend");
    filter("os", context.platform.os, "os");
    if (context.platform.rocm_arch.empty() && std::all_of(eligible.begin(), eligible.end(), [&](size_t i) { return install["cases"][i].contains("arch"); })) {
        if (install.contains("errors") && install["errors"].contains("missing_arch")) throw std::runtime_error(definition_->required_value("/install/errors/missing_arch", values).get<std::string>());
    }
    filter("arch", context.platform.rocm_arch, "arch");
    const size_t index = eligible.front();
    const auto& record = install["cases"][index];
    const std::string pointer = "/install/cases/" + std::to_string(index);
    InstallParams result{record["repo"], definition_->required_value(pointer + "/filename", values).get<std::string>(), ""};
    if (record.contains("version_override")) result.version_override = definition_->required_value(pointer + "/version_override", values).get<std::string>();
    result.sha256 = record.value("sha256", "");
    return result;
}

}
