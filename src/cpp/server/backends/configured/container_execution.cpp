#include "../../system_info_utils.h"
#include "backend_definition_runner_internal.h"
#include "lemon/backends/configured/container_execution.h"
#include "lemon/system_info.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

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
    std::set<std::string> named_destinations;
    for (const auto& mount : program.launch.mounts) {
        if (mount.condition && !evaluate(*mount.condition).value_or(true).get<bool>()) continue;
        const auto artifact = std::find_if(program.artifacts.begin(), program.artifacts.end(),
            [&](const auto& item) { return item.name == mount.artifact; });
        if (artifact == program.artifacts.end() || !frame.data.values.at("artifacts").contains(mount.artifact))
            throw std::invalid_argument("Mount source is outside the artifact inventory");
        const auto path = frame.data.values.at("artifacts").at(mount.artifact).get<std::string>();
        if (mount.destination != "/mnt/models" || (mount.layout != "artifact_id" && mount.layout != "basename_numbered"))
            throw std::invalid_argument("Unsupported container mount layout");
        if (mount.layout == "basename_numbered" && plan.container_path_rewrites.count(path)) {
            frame.data.values["artifacts"][mount.artifact] = plan.container_path_rewrites.at(path);
            continue;
        }
        auto named = utils::path_from_utf8(path);
        if (!named.has_filename()) named = named.parent_path();
        const auto filename = utils::path_to_utf8(named.filename());
        const auto base = mount.destination + (mount.layout == "artifact_id" ? "/" + mount.id : "");
        auto target = base + "/" + filename;
        if (mount.layout == "basename_numbered") {
            for (unsigned number = 2; named_destinations.count(target); ++number) {
                if (number > 4096) throw std::invalid_argument("Container mount collision limit exceeded");
                target = base + "/" + std::to_string(number) + "/" + filename;
            }
        }
        named_destinations.insert(target);
        auto mounts = runtime::ArtifactServices::container_mounts(path, mount.expand, target);
        if (mounts.empty()) throw std::invalid_argument("Empty container artifact mount");
        for (auto& item : mounts) {
            if (!destinations.insert(item.destination).second) throw std::invalid_argument("Duplicate container mount destination");
            plan.container->mounts.push_back(std::move(item));
        }
        plan.container_path_rewrites.emplace(path, target);
        frame.data.values["artifacts"][mount.artifact] = target;
    }
}

std::vector<ContainerGpuNode> read_container_gpu_nodes(const std::filesystem::path& root) {
    std::vector<ContainerGpuNode> result;
    unsigned count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (++count > 256) throw std::runtime_error("Container GPU topology exceeds node limit");
        const auto name = entry.path().filename().string();
        if (name.empty() || name.size() > 8 || name.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("Invalid KFD node identifier");
        const auto properties = entry.path() / "properties";
        if (std::filesystem::file_size(properties) > 65536) throw std::runtime_error("KFD properties exceed limit");
        std::ifstream file(properties);
        if (!file) throw std::runtime_error("Cannot read KFD properties");
        std::string text(65537, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (file.bad() || file.gcount() > 65536) throw std::runtime_error("KFD properties exceed read limit");
        text.resize(static_cast<std::size_t>(file.gcount()));
        std::istringstream input(text);
        std::string key, value, architecture;
        unsigned lines = 0;
        bool found = false;
        while (input >> key >> value) {
            if (++lines > 1024) throw std::runtime_error("KFD properties exceed record limit");
            if (key != "gfx_target_version") continue;
            if (found) throw std::runtime_error("Duplicate KFD architecture");
            found = true;
            if (value == "0") continue;
            architecture = system_info_detail::gfx_target_version_to_arch(value);
            if (architecture.empty()) throw std::runtime_error("Invalid KFD architecture");
        }
        if (!found || input.bad()) throw std::runtime_error("Missing KFD architecture");
        if (!architecture.empty()) result.push_back({static_cast<unsigned>(std::stoul(name)), architecture});
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.node < b.node; });
    return result;
}

std::vector<ContainerGpuNode> EffectLaunchServices::container_gpu_nodes() const {
#ifdef __linux__
    return read_container_gpu_nodes("/sys/class/kfd/kfd/topology/nodes");
#else
    throw std::runtime_error("Container GPU selection requires Linux");
#endif
}

std::string EffectLaunchServices::container_gpu_properties(unsigned node) const {
#ifdef __linux__
    const auto path = std::filesystem::path("/sys/class/kfd/kfd/topology/nodes") / std::to_string(node) / "properties";
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot read container GPU properties");
    std::string text(65537, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (input.bad() || input.gcount() > 65536) throw std::runtime_error("Container GPU properties exceed limit");
    text.resize(static_cast<std::size_t>(input.gcount()));
    return text;
#else
    (void)node;
    throw std::runtime_error("Container GPU properties require Linux");
#endif
}

bool EffectLaunchServices::container_supports_arch(const std::string& recipe, const std::string& backend, const std::string& arch) const {
    return SystemInfo::backend_supports_arch(recipe, backend, arch);
}

void supply_container_gpu(const utils::ContainerRunSpec& spec,
    std::vector<std::pair<std::string, std::string>>& environment, const LaunchServices& services) {
    if (std::find(spec.devices.begin(), spec.devices.end(), "/dev/kfd") == spec.devices.end()) return;
    auto nodes = services.container_gpu_nodes();
    if (nodes.empty() || nodes.size() > 256) throw std::runtime_error("Container requires a bounded GPU topology");
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) { return a.node < b.node; });
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if ((i && nodes[i - 1].node == node.node) || node.architecture.size() < 4 || node.architecture.size() > 16 ||
            node.architecture.rfind("gfx", 0) != 0 || node.architecture.find_first_not_of("0123456789abcdef", 3) != std::string::npos)
            throw std::runtime_error("Invalid container GPU topology");
    }
    for (std::size_t ordinal = 0; ordinal < nodes.size(); ++ordinal) {
        if (!services.container_supports_arch(spec.recipe, spec.backend, nodes[ordinal].architecture)) continue;
        environment.emplace_back("HIP_VISIBLE_DEVICES", std::to_string(ordinal));
        return;
    }
    throw std::runtime_error("No supported container GPU");
}

}
