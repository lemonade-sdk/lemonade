#include "lemon/backends/backend_option_contract.h"
#include "lemon/backends/configured/backend_hook_registry.h"
#include "lemon/backends/configured/launch_planner.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/recipe_arg_resolver.h"

#include <algorithm>
#include <filesystem>
#include <set>

namespace lemon::backends {

PlatformInfo native_platform() {
    PlatformInfo platform;
#ifdef _WIN32
    platform.os = "windows";
#elif defined(__APPLE__)
    platform.os = "macos";
#else
    platform.os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    platform.machine_arch = "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    platform.machine_arch = "x86_64";
#endif
    return platform;
}

std::vector<std::pair<std::string, std::string>> EffectLaunchServices::environment(
    const std::string& preset, const std::string& executable, const std::string& backend, const std::string& category) const {
    std::vector<std::pair<std::string, std::string>> result;
    if (preset == "inherit") return result;
    if (preset == "posix_executable_loader_path") {
        if (platform().os == "windows") return result;
        const auto inherited = effects_.inherited_posix_loader();
        auto value = std::filesystem::path(executable).parent_path().string();
        if (inherited && !inherited->empty()) value += ":" + *inherited;
        result.emplace_back("LD_LIBRARY_PATH", value);
        return result;
    }
    if (preset == "bundled_accelerator_runtime" && backend != "rocm" && backend != "cuda") return result;
    std::string directories;
    if (preset == "bundled_accelerator_runtime" && backend == "rocm") {
        const auto arch = effects_.rocm_arch();
        if (!arch.empty()) directories = effects_.runtime_directories(arch);
    }
    const bool posix = preset == "executable_loader_path" && posix_executable_loader_path_;
    result.push_back(make_loader_path_override(std::filesystem::path(executable).parent_path().string(),
        directories, posix ? effects_.inherited_posix_loader() : effects_.inherited_loader(), !posix && platform().os == "windows"));
    if (preset == "bundled_accelerator_runtime" && backend == "cuda") effects_.append_cuda(result, category);
    return result;
}
long EffectLaunchServices::default_timeout_seconds() const { return utils::HttpClient::get_default_timeout(); }

void LaunchPlanner::require_platform(const BackendDefinition& definition, const LaunchServices& services) const {
    const auto& launch = definition.records.at("launch");
    if (!launch.contains("requires")) return;
    const auto& required = launch["requires"];
    const auto platform = services.platform();
    auto allowed = [&](const char* axis, const std::string& value) {
        return !required.contains(axis) || std::find(required[axis].begin(), required[axis].end(), value) != required[axis].end();
    };
    if (!allowed("os", platform.os) || !allowed("machine_arch", platform.machine_arch)) throw std::runtime_error(required["error"].get<std::string>());
}

ResolvedLaunchInputs LaunchPlanner::prepare(const BackendDefinition& definition, const BackendSpec& spec,
                                           const LoadContext& context, LaunchServices& services) const {
    const auto& launch = definition.records.at("launch");
    const auto& metadata = definition.records.at("metadata");
    ResolvedLaunchInputs inputs;
    auto& values = inputs.values;
    values = {{"options", context.options.to_json()}, {"model", {{"name", context.model_name}, {"checkpoint", context.model_info.checkpoint()}}},
              {"artifacts", json::object()}, {"launch", {{"ports", json::object()}}}};
    if (definition.origin == DefinitionOrigin::External) {
        values["options"] = context.options.to_resolved_json();
        for (const auto& option : metadata.at("options")) {
            const std::string name = option["name"];
            const auto value = context.options.has_option(name) ? context.options.get_option(name) : option["default_value"];
            const auto error = validate_option_contract(option, value);
            if (!error.empty()) throw std::invalid_argument("Invalid option " + name + ": " + error);
            values["options"][name] = value;
        }
    }
    std::string custom_text;
    if (launch.contains("custom_args")) {
        const std::string option = launch["custom_args"]["option"];
        custom_text = context.options.get_option(option).get<std::string>();
        values["options"][option] = custom_text;
    }
    if (metadata.value("uses_ctx_size", false)) values["options"]["ctx_size"] = context.options.get_option("ctx_size").get<int>();
    std::string level = context.log_level;
    if (metadata.at("log_level_policy") == "debug_flag" && level != "debug") level = "info";
    values["launch"]["log_level"] = level;
    std::string backend;
    const auto& order = launch.at("prepare_order");
    const bool install_phase = std::find(order.begin(), order.end(), "install") != order.end();
    for (const auto& phase : order) {
        if (phase == "artifacts") {
            const auto artifacts = definition.records.value("artifacts", json::object());
            for (const auto& [name, artifact] : artifacts.items()) {
                const std::string role = artifact.at("role");
                auto path = context.model_info.resolved_path(role);
                bool valid;
                if (artifact.value("absolute_checkpoint_fallback", false)) {
                    if (path.empty() || !services.exists(path)) {
                        const auto raw = context.model_info.checkpoint(role);
                        if (!raw.empty() && std::filesystem::path(raw).is_absolute() && services.exists(raw)) path = raw;
                    }
                    valid = !path.empty() && services.exists(path);
                } else valid = !path.empty() && (artifact.value("check", "exists") == "nonempty" || services.exists(path));
                values["artifacts"][name] = path;
                json::json_pointer pointer("/artifacts");
                pointer /= name; pointer /= "missing_error";
                if (!valid) throw std::runtime_error(definition.required_value(pointer.to_string(), values).get<std::string>());
            }
        } else if (phase == "backend") {
            const auto& selection = launch.at("backend");
            if (selection.contains("fixed")) backend = selection["fixed"].is_string() ? selection["fixed"] : selection["fixed"].at(services.platform().os);
            else {
                const std::string option = selection.at("option");
                backend = context.options.get_option(option).get<std::string>();
                values["options"][option] = backend;
                if (backend.empty()) backend = first_supported_backend(services.supported_backends(definition.recipe),
                    selection["on_empty"]["operation"], selection["on_empty"]["target"]);
                services.validate_backend(definition.recipe, backend);
            }
            values["launch"]["backend"] = backend;
            if (context.backend_selected) context.backend_selected();
        } else if (phase == "install") {
            services.install_backend(definition.recipe, backend);
        } else if (phase == "hook") {
            const std::string category = metadata["log_category"];
            values["launch"]["hook"] = installed_backend_hooks().target<LaunchHook>(definition.records["hooks"]["launch"]["id"].get<std::string>(), HookStage::Launch)({values, context.model_info, category});
        } else if (phase == "program") {
            const BackendBinaryServices binary = {
                [&](const auto&, const auto&) { return services.external_binary(spec, backend); },
                [&](const auto& path) { return services.exists(path); },
                [&](const auto&, const auto&) { services.install_backend(definition.recipe, backend); },
                [&](const auto&, const auto&) { return services.managed_binary(spec, backend); }
            };
            if (definition.records["install"]["strategy"] == "external_only") {
                inputs.executable = services.external_binary(spec, backend);
                if (inputs.executable.empty() || !services.exists(inputs.executable)) throw std::runtime_error("External backend executable is required");
            } else if (launch["program"]["source"] == "external_or_managed") inputs.executable = resolve_external_or_managed_binary(spec, backend, binary);
            else {
                if (!install_phase) services.install_backend(definition.recipe, backend);
                inputs.executable = services.managed_binary(spec, backend);
            }
            values["launch"]["executable"] = inputs.executable;
            if (launch["program"].contains("missing_error") && inputs.executable.empty()) throw std::runtime_error(definition.required_value("/launch/program/missing_error", values).get<std::string>());
        } else {
            for (const auto& port : launch.at("ports")) {
                const int number = services.allocate_port();
                if (port["reject_zero"] == true && number == 0) throw std::runtime_error(port["error"].get<std::string>());
                values["launch"]["ports"][port["name"].get<std::string>()] = number;
            }
        }
        if (context.phase_prepared) context.phase_prepared(phase.get<std::string>(), values);
    }
    for (const auto& option : metadata.at("options")) {
        const std::string name = option["name"];
        if (!values["options"].contains(name)) values["options"][name] = context.options.has_option(name) ? context.options.get_option(name) : option["default_value"];
    }
    if (launch.contains("custom_args")) {
        const auto& custom = launch["custom_args"];
        if (!custom_text.empty()) {
            const auto error = utils::validate_custom_args(custom_text, custom["reserved"].get<std::set<std::string>>());
            if (!error.empty()) throw std::invalid_argument(custom["error_prefix"].get<std::string>() + error);
        }
        std::vector<utils::RuntimeArgDefault> defaults;
        for (const auto& item : custom.value("defaults", json::array())) defaults.push_back({item["args"], item["flag"], item.value("aliases", std::vector<std::string>{})});
        const auto resolved = defaults.empty() ? custom_text : utils::append_runtime_arg_defaults(custom_text, defaults);
        if (!resolved.empty()) inputs.custom_arguments = utils::parse_custom_args(resolved);
    }
    const auto platform = services.platform();
    values["platform"] = {{"os", platform.os}, {"machine_arch", platform.machine_arch}, {"rocm_arch", platform.rocm_arch}};
    const auto& readiness = launch.at("readiness");
    values["launch"]["readiness_timeout"] = readiness["timeout"] == "global" ? services.default_timeout_seconds() : readiness["timeout"].get<long>();
    inputs.environment = services.environment(launch["environment"]["preset"], inputs.executable, backend, metadata["log_category"]);
    if (context.phase_prepared) {
        values["launch"]["environment"] = inputs.environment;
        context.phase_prepared("environment", values);
    }
    return inputs;
}

LaunchPlan LaunchPlanner::plan(const BackendDefinition& definition, const ResolvedLaunchInputs& inputs) const {
    const auto& launch = definition.records.at("launch");
    LaunchPlan plan;
    plan.command.program = inputs.executable;
    plan.command.env = inputs.environment;
    plan.command.port = inputs.values.at("launch").at("ports").at(launch["readiness"]["port"].get<std::string>());
    plan.command.ready_endpoint = launch["readiness"]["path"];
    plan.readiness_timeout_seconds = inputs.values["launch"]["readiness_timeout"];
    plan.watchdog_enabled = launch["watchdog"]["policy"] == "standard";
    const auto& levels = launch["output"]["inherit_levels"];
    plan.output = {std::find(levels.begin(), levels.end(), inputs.values["launch"]["log_level"]) != levels.end(), launch["output"]["filter"].get<bool>()};
    if (launch["working_directory"] == "executable_directory") plan.working_directory = std::filesystem::path(inputs.executable).parent_path().string();
    for (size_t i = 0; i < launch["argv"].size(); ++i) {
        const auto& group = launch["argv"][i];
        const std::string path = "/launch/argv/" + std::to_string(i);
        if (group.contains("when") && !definition.required_value(path + "/when", inputs.values).get<bool>()) continue;
        for (size_t j = 0; j < group["args"].size(); ++j) plan.command.args.push_back(definition.required_value(path + "/args/" + std::to_string(j), inputs.values).get<std::string>());
    }
    plan.command.args.insert(plan.command.args.end(), inputs.custom_arguments.begin(), inputs.custom_arguments.end());
    const auto entries = launch["environment"].value("set", json::array());
    std::vector<std::pair<std::string, std::string>> assigned;
    for (size_t i = 0; i < entries.size(); ++i) {
        assigned.emplace_back(entries[i]["name"], definition.required_value("/launch/environment/set/" + std::to_string(i) + "/value", inputs.values).get<std::string>());
    }
    if (launch["environment"].value("order", "preset_first") == "set_first") {
        for (const auto& entry : plan.command.env) {
            if (std::none_of(assigned.begin(), assigned.end(), [&](const auto& set) { return set.first == entry.first; })) assigned.push_back(entry);
        }
        plan.command.env = std::move(assigned);
    } else for (const auto& [name, value] : assigned) {
        auto found = std::find_if(plan.command.env.begin(), plan.command.env.end(), [&](const auto& entry) { return entry.first == name; });
        if (found == plan.command.env.end()) plan.command.env.emplace_back(name, value);
        else found->second = value;
    }
    return plan;
}

}
