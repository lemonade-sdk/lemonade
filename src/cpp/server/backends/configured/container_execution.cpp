#include "backend_definition_runner_internal.h"
#include "lemon/backends/configured/container_execution.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace lemon::backends {

const execution::ExecutionCase& selected_execution_case(const execution::Program& program, const std::string& backend) {
    const execution::ExecutionCase* selected = nullptr;
    for (const auto& item : program.launch.execution_cases) {
        if (std::find(item.backends.begin(), item.backends.end(), backend) == item.backends.end()) continue;
        if (selected) throw std::invalid_argument("Ambiguous execution case");
        selected = &item;
    }
    if (!selected) throw std::invalid_argument("Missing execution case");
    if (selected->kind != "native" && selected->kind != "container") throw std::invalid_argument("Unknown execution kind");
    if (selected->kind == "container") container_policy(program, *selected);
    return *selected;
}

ContainerPolicy container_policy(const execution::Program& program, const execution::ExecutionCase& selected) {
    const auto& doc = program.normalized;
    if (selected.kind != "container" || selected.container.empty() || !doc.contains("containers") ||
        !doc.at("containers").contains(selected.container) || selected.backends != std::vector<std::string>{selected.container})
        throw std::invalid_argument("Container execution case has no matching policy");
    const auto& policy = doc.at("containers").at(selected.container);
    const auto& pin = policy.at("pin");
    return {policy.at("repository"), {pin.at("source"), pin.at("recipe"), pin.at("backend")},
        policy.at("devices").get<std::vector<std::string>>(), policy.at("cap_add").get<std::vector<std::string>>(),
        policy.at("policy"), policy.at("network"), policy.at("teardown"), policy.at("stop_grace_seconds")};
}

std::optional<ContainerPolicy> configured_container_policy(const BackendSpec& spec, const std::string& backend) {
    if (!spec.executable_definition) return {};
    const auto& compiled = spec.executable_definition->compiled();
    const auto& program = execution::Inspection::program(compiled);
    if (!container_capable(program.normalized)) return {};
    if (spec.definition || !AuthorityInspection::permits_execution(compiled.authorized(), program.normalized) ||
        spec.effective_public_recipe() != program.normalized.at("recipe") || spec.effective_pin_recipe() != spec.effective_public_recipe())
        throw std::logic_error("Container installation has no matching execution owner");
    const auto& selected = selected_execution_case(program, backend);
    return selected.kind == "container" ? std::optional<ContainerPolicy>(container_policy(program, selected)) : std::nullopt;
}

std::string effective_container_pin(const ContainerPolicy& policy, const json& versions, const std::string& requested) {
    if (policy.pin.source != "backend_versions" || !versions.contains(policy.pin.recipe) ||
        !versions.at(policy.pin.recipe).is_object() || !versions.at(policy.pin.recipe).contains(policy.pin.backend) ||
        !versions.at(policy.pin.recipe).at(policy.pin.backend).is_string())
        throw std::invalid_argument("Missing container backend_versions pin");
    const auto pinned = versions.at(policy.pin.recipe).at(policy.pin.backend).get<std::string>();
    const auto effective = requested.empty() || requested == "builtin" ? pinned : requested;
    if (!utils::parse_container_pin(effective)) throw std::invalid_argument("Container pin requires <tag>@sha256:<64 lowercase hex>");
    return effective;
}

bool valid_container_image(const std::string& repository, const std::string& image) {
    const auto prefix = repository + "@";
    return utils::ContainerManager::allowed_image(repository) && image.rfind(prefix, 0) == 0 &&
        utils::parse_container_pin("pin@" + image.substr(prefix.size())).has_value();
}

utils::ContainerRunSpec container_run_spec(const execution::Program& program, const execution::ExecutionCase& selected, const std::string& model) {
    const auto policy = container_policy(program, selected);
    if (policy.policy != "lemonade-isolated-v1" || policy.network != "private_internal" ||
        policy.teardown != "stop_remove_reap" || policy.stop_grace_seconds != 10 ||
        (selected.program.at("source") != "image_command" && selected.program.at("source") != "image_entrypoint"))
        throw std::invalid_argument("Unsupported sealed container execution policy");
    utils::ContainerRunSpec spec;
    spec.recipe = program.normalized.at("recipe");
    spec.backend = selected.container;
    spec.model = model;
    spec.name = utils::ContainerManager::container_name(spec.recipe, spec.backend, model);
    spec.network = spec.name;
    spec.devices = policy.devices;
    spec.cap_add = policy.cap_add;
    return spec;
}

void materialize_container_mounts(const execution::Program& program, runtime::Frame& frame, LaunchPlan& plan,
    const std::function<std::optional<json>(const std::string&)>& evaluate) {
    std::set<std::string> destinations;
    for (const auto& mount : program.launch.mounts) {
        if (mount.condition && !evaluate(*mount.condition).value_or(true).get<bool>()) continue;
        const auto artifact = std::find_if(program.artifacts.begin(), program.artifacts.end(),
            [&](const auto& item) { return item.name == mount.artifact; });
        if (artifact == program.artifacts.end() || !frame.data.values.at("artifacts").contains(mount.artifact))
            throw std::invalid_argument("Mount source is outside the artifact inventory");
        const auto path = frame.data.values.at("artifacts").at(mount.artifact).get<std::string>();
        const auto target = mount.destination + "/" + mount.id + "/" + utils::path_to_utf8(utils::path_from_utf8(path).filename());
        auto mounts = runtime::ArtifactServices::container_mounts(path, mount.expand, target);
        if (mounts.empty()) throw std::invalid_argument("Empty container artifact mount");
        for (auto& item : mounts) {
            if (!destinations.insert(item.destination).second) throw std::invalid_argument("Duplicate container mount destination");
            plan.container->mounts.push_back(std::move(item));
        }
        frame.data.values["artifacts"][mount.artifact] = target;
    }
}

}
