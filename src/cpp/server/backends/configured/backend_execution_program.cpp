#include "backend_definition_execution_internal.h"
#include "lemon/backends/backend_option_contract.h"

#include <algorithm>
#include <set>

namespace lemon::backends::execution {

ModelType model_type(const std::string& name) {
    static const std::map<std::string, ModelType> types = {{"audio-generation", ModelType::AUDIO_GENERATION},
        {"chat", ModelType::LLM}, {"classification", ModelType::CLASSIFICATION}, {"embedding", ModelType::EMBEDDING},
        {"image", ModelType::IMAGE}, {"llm", ModelType::LLM}, {"mesh", ModelType::MESH}, {"reranking", ModelType::RERANKING},
        {"transcription", ModelType::TRANSCRIPTION}, {"tts", ModelType::TTS}};
    const auto found = types.find(name);
    require(found != types.end(), "invalid_model_type", "/operations");
    return found->second;
}

void bind_options(Program& program, Budget& budget) {
    const auto& options = program.normalized["metadata"]["options"];
    std::set<std::string> names, keys;
    for (std::size_t i = 0; i < options.size(); ++i) {
        const auto& option = options[i];
        const auto p = "/metadata/options/" + std::to_string(i);
        budget.copy(option);
        budget.keep(sizeof(OptionBinding) + 256);
        const std::string id = option["id"], name = option["name"], type = option["value_type"];
        const auto key = backend_option_config_key(name);
        budget.keep(v2::checked_product(id.size() + name.size() + key.size() + type.size(), 4));
        require(program.slots.emplace(id, i).second && names.insert(name).second && keys.insert(key).second, "option_collision", p);
        require(validate_option_contract(option, option["default_value"]).empty(), "invalid_option_contract", p);
        if (option.contains("minimum") && option.contains("maximum")) require(option["minimum"] <= option["maximum"], "invalid_option_contract", p);
        for (const auto& value : option.value("enum", Json::array())) {
            budget.work(0);
            require(validate_option_contract(option, value).empty(), "invalid_option_contract", p);
        }
        for (const auto& value : option.value("unset_values", Json::array({nullptr})))
            require(!value.is_structured(), "invalid_option_contract", p);
        program.bindings.push_back({id, name, key, type, option});
    }
}

namespace {
std::size_t slot(const Program& program, const Json& value, const std::string& pointer) {
    const auto found = program.slots.find(value.get<std::string>());
    require(found != program.slots.end() && program.bindings[found->second].type == "string", "invalid_option_binding", pointer);
    return found->second;
}
void pointer(const std::string& value, const std::string& at) {
    try { Json::json_pointer parsed(value); require(!value.empty() && value[0] == '/', "invalid_pointer", at); }
    catch (const Json::exception&) { throw Failure{"invalid_pointer", at}; }
}
bool prefix(const std::string& left, const std::string& right) { return left == right || left.rfind(right + "/", 0) == 0 || right.rfind(left + "/", 0) == 0; }
}

void compile_programs(Program& program, Budget& budget) {
    const auto& doc = program.normalized;
    const auto& launch = doc["launch"];
    budget.copy(launch);
    budget.copy(launch);
    budget.copy(doc["install"]);
    budget.copy(doc["install"]);
    program.launch.records = launch;
    program.install.records = doc["install"];
    program.install.strategy = doc["install"]["strategy"];
    program.launch.prepare_order = launch["prepare_order"].get<std::vector<std::string>>();
    program.launch.program_source = launch["program"]["source"];
    program.launch.install_policy = launch["program"]["install_policy"];
    if (launch["backend"].contains("fixed")) program.launch.fixed_backend = launch["backend"]["fixed"];
    program.launch.environment_preset = launch["environment"]["preset"];
    program.launch.environment_order = launch["environment"]["order"];
    for (std::size_t i = 0; i < launch["environment"]["set"].size(); ++i) {
        const auto& name = launch["environment"]["set"][i]["name"].get_ref<const std::string&>();
        budget.keep(name.size() + 192);
        program.launch.environment_set.push_back({name, "/launch/environment/set/" + std::to_string(i) + "/value"});
    }
    if (launch["backend"].contains("option")) program.launch.backend_slot = slot(program, launch["backend"]["option"], "/launch/backend/option");
    if (launch.contains("custom_args")) program.launch.custom_args_slot = slot(program, launch["custom_args"]["option"], "/launch/custom_args/option");
    const auto& cwd = launch["working_directory"];
    for (const auto* os : {"windows", "linux", "macos"}) {
        budget.keep(128);
        program.launch.working_directory[os] = cwd.is_string() ? cwd.get<std::string>() : cwd.at(os).get<std::string>();
    }
    if (launch.contains("context_floor")) for (const auto& [name, floor] : launch["context_floor"].items()) {
        budget.keep(64);
        program.launch.context_floor.emplace(model_type(name), floor.get<std::uint64_t>());
    }
    std::map<std::string, int> ports;
    for (const auto& port : launch["ports"]) {
        budget.work(0);
        budget.keep(port["name"].get_ref<const std::string&>().size() + 128);
        const std::string name = port["name"], strategy = port.value("strategy", "standard");
        require(!ports.count(name), "duplicate_port", "/launch/ports");
        require((strategy == "probe" || (!port.contains("span") && !port.contains("attempts"))) &&
            (strategy == "offset" || (!port.contains("base") && !port.contains("offset"))), "invalid_port_strategy", "/launch/ports");
        if (strategy == "offset") require(port.contains("base") && port.contains("offset") &&
            ports.count(port.value("base", "")) && port["offset"].get<int>() < ports[port["base"].get<std::string>()], "invalid_port_strategy", "/launch/ports");
        ports.emplace(name, strategy == "probe" ? port.value("span", 1) : 0);
        budget.copy(port);
        budget.keep(sizeof(Port));
        program.launch.ports.push_back({name, strategy, port["reject_zero"], port["error"],
            port.value("span", 1u), port.value("attempts", 1u), port.value("base", ""), port.value("offset", 0u)});
    }
    for (std::size_t i = 0; i < launch["argv"].size(); ++i) {
        const auto& record = launch["argv"][i];
        const auto p = "/launch/argv/" + std::to_string(i);
        budget.copy(record);
        budget.keep(sizeof(Argument) + p.size() * 4 + 256);
        Argument argument;
        argument.id = record["id"];
        argument.condition = p + "/when";
        if (record.contains("flag")) {
            argument.flag = record["flag"];
            argument.reserve = record["reserve"];
            argument.aliases = record["aliases"].get<std::vector<std::string>>();
            if (record.contains("value")) argument.values.push_back(p + "/value");
        } else for (std::size_t j = 0; j < record["args"].size(); ++j) {
            budget.keep(p.size() + 64);
            argument.values.push_back(p + "/args/" + std::to_string(j));
        }
        program.launch.argv.push_back(std::move(argument));
    }
    if (launch.contains("custom_args")) for (std::size_t i = 0; i < launch["custom_args"]["defaults"].size(); ++i) {
        const auto& record = launch["custom_args"]["defaults"][i];
        budget.copy(record);
        budget.keep(sizeof(CustomDefault) + 256);
        program.launch.defaults.push_back({record["id"], record["args"], record["flag"], record["stage"],
            "/launch/custom_args/defaults/" + std::to_string(i) + "/when", record["respect_merge_args"], record["aliases"].get<std::vector<std::string>>()});
    }
    if (doc.contains("artifacts")) for (const auto& [name, artifact] : doc["artifacts"].items()) {
        const auto p = "/artifacts/" + v2::pointer_token(name);
        budget.copy(artifact);
        budget.keep(sizeof(ArtifactProgram) + name.size() + p.size() * 2 + 64);
        program.artifacts.push_back({name, artifact["role"], p + "/required_when", p + "/missing_error", artifact});
        require(artifact["strategy"] == "resolved_path" || (artifact["strategy"] == "hook" &&
            std::any_of(program.hooks.begin(), program.hooks.end(), [&](const auto& hook) {
                return hook.slot.owner_kind == SlotOwnerKind::Artifact && hook.slot.owner_id == name &&
                    hook.contract == typed::Contract::Artifacts;
            })), "unsupported_execution_feature", p);
        require(artifact.contains("missing_error"), "invalid_artifact", p);
        const auto cache = artifact.value("cache_resolution", "default");
        if (cache == "sentinel_directory" || cache == "sentinel_file" || artifact.value("import_resolution", "default") == "sentinel_directory") {
            const auto sentinel = artifact.value("sentinel", "");
            require(!sentinel.empty() && sentinel != "." && sentinel != ".." && sentinel.find_first_of("/\\:") == std::string::npos && sentinel.find('\0') == std::string::npos && artifact.value("selection", "") == "first_existing", "invalid_artifact", p);
        }
        require(!artifact.contains("files_from") || (artifact["role"] == "main" && !artifact.contains("files") && artifact.contains("files_error")), "invalid_artifact", p);
        require(!artifact.contains("files_error") || artifact.contains("files_from"), "invalid_artifact", p);
    }
    require(doc["install"]["strategy"] != "hook", "unsupported_execution_feature", "/install/strategy");
    const auto& cases = doc["install"]["cases"];
    for (std::size_t i = 0; i < cases.size(); ++i) {
        budget.copy(cases[i]);
        budget.keep(sizeof(InstallCase) + 256);
        const auto path = "/install/cases/" + std::to_string(i);
        InstallCase compiled;
        compiled.id = cases[i]["id"];
        compiled.repo = cases[i]["repo"];
        compiled.filename = path + "/filename";
        if (cases[i].contains("version_override")) compiled.version_override = path + "/version_override";
        if (cases[i].contains("sha256")) compiled.sha256 = cases[i]["sha256"];
        for (const auto* axis : {"backend", "os", "machine_arch", "arch"}) if (cases[i].contains(axis))
            compiled.selectors.emplace(axis, cases[i][axis].get<std::vector<std::string>>());
        program.install.cases.push_back(std::move(compiled));
        require(!(doc["install"].value("supports_split_archive", false) && cases[i].contains("sha256")), "unsupported_execution_feature", "/install/cases");
        for (std::size_t j = 0; j < i; ++j) {
            budget.work(0);
            bool overlaps = true;
            for (const auto* axis : {"backend", "os", "machine_arch", "arch"}) if (cases[i].contains(axis) && cases[j].contains(axis)) {
                bool intersects = false;
                for (const auto& left : cases[i][axis]) for (const auto& right : cases[j][axis]) { budget.work(0); intersects |= left == right; }
                overlaps &= intersects;
            }
            require(!overlaps, "selector_overlap", "/install/cases/" + std::to_string(i));
        }
    }
    const auto& install = program.install.records;
    const bool full = install.value("repository_result", "repository_only") == "full_params";
    const bool early = install.value("fact_timing", "selected_case") == "before_platform";
    const auto declared = install.value("repository_facts", std::vector<std::string>{});
    const auto unmanaged = install.value("unmanaged_backends", std::vector<std::string>{});
    const auto fallback = install.value("backend_fallback", "");
    require((full || declared.empty()) && (program.install.strategy == "github_release" ||
        (!full && !early && declared.empty() && unmanaged.empty() && fallback.empty())), "invalid_install_policy", "/install");
    const bool fact_hook = std::any_of(program.hooks.begin(), program.hooks.end(), [](const auto& hook) {
        return hook.contract == typed::Contract::InstallContext;
    });
    require(declared.empty() || fact_hook, "invalid_install_policy", "/install/repository_facts");
    std::map<std::string, std::set<std::string>> backend_facts;
    std::set<std::string> used_facts, backends;
    for (const auto& record : program.install.cases) {
        std::set<std::string> facts;
        const auto visit = [&](const auto& self, const Expression& expression) -> void {
            budget.work(0);
            for (const auto* name : {"therock_version", "rocm_asset_family", "cuda_architecture"})
                if (expression.reference == std::string("/install/") + name) facts.insert(name);
            if (early) require(expression.kind != Expression::Kind::When && expression.kind != Expression::Kind::Coalesce &&
                expression.kind != Expression::Kind::All && expression.kind != Expression::Kind::Any,
                "ambiguous_install_facts", record.filename);
            for (const auto& child : expression.children) self(self, child);
        };
        visit(visit, program.expressions.at(record.filename));
        if (record.version_override) visit(visit, program.expressions.at(*record.version_override));
        used_facts.insert(facts.begin(), facts.end());
        if (full) for (const auto& fact : facts) require(std::find(declared.begin(), declared.end(), fact) != declared.end(),
            "undeclared_repository_fact", "/install/repository_facts");
        const auto group = record.selectors.find("backend");
        if (early) require(group != record.selectors.end(), "ambiguous_install_facts", "/install/cases");
        if (group != record.selectors.end()) for (const auto& backend : group->second) {
            budget.keep(backend.size() + 256);
            backends.insert(backend);
            const auto found = backend_facts.find(backend);
            if (early) require(found == backend_facts.end() || found->second == facts, "ambiguous_install_facts", "/install/cases");
            backend_facts.emplace(backend, facts);
            if (early) for (const auto& fact : facts) require((fact == "therock_version" && backend == "rocm-stable") ||
                (fact == "rocm_asset_family" && backend == "rocm-nightly") || (fact == "cuda_architecture" && backend == "cuda"),
                "invalid_install_fact_backend", "/install/cases");
        }
    }
    for (const auto& fact : declared) require(used_facts.count(fact), "unused_repository_fact", "/install/repository_facts");
    for (const auto& backend : unmanaged) require(!backends.count(backend), "invalid_install_policy", "/install/unmanaged_backends");
    require(fallback.empty() || backends.count(fallback), "invalid_install_policy", "/install/backend_fallback");
    for (std::size_t i = 0; i < doc["operations"].size(); ++i) {
        const auto& record = doc["operations"][i];
        const auto p = "/operations/" + std::to_string(i);
        budget.copy(record);
        budget.keep(sizeof(OperationProgram) + sizeof(OperationDescription) + 256);
        require(record.contains("transport") && !record.contains("handler"), "unsupported_execution_feature", p);
        require(!record.contains("formats") && !record.contains("format_metadata"), "unsupported_execution_feature", p);
        OperationProgram op;
        op.id = record["id"];
        BackendOperation operation = BackendOperation::Count;
        require(parse_backend_operation(record["operation"].get<std::string>(), operation), "unknown_operation", p);
        const auto delivery = record["delivery"] == "buffered" ? OperationDelivery::Buffered : record["delivery"] == "streaming" ? OperationDelivery::Streaming : OperationDelivery::Realtime;
        ModelTypeMask mask = 0;
        for (const auto& name : record["model_modes"]) {
            budget.work(0);
            require(name != "llm", "invalid_model_type", p);
            const auto bit = name == "*" ? k_all_operation_model_types : operation_model_type_mask(model_type(name.get<std::string>()));
            require(!(mask & bit), "operation_overlap", p);
            mask |= bit;
        }
        require(mask && valid_operation_delivery(operation, delivery), "unsupported_execution_feature", p);
        op.description = {operation, delivery, mask, OperationProvenance::Implemented};
        for (const auto& previous : program.operations) {
            budget.work(0);
            require(previous.description.operation != operation || previous.description.delivery != delivery || !(previous.description.model_types & mask), "operation_overlap", p);
        }
        const auto& transport = record["transport"];
        op.kind = transport["kind"]; op.method = transport["method"]; op.path = transport["path"]; op.port = transport["port"];
        const bool json_operation = operation <= BackendOperation::Reranking || operation == BackendOperation::Slots || operation == BackendOperation::SlotAction || operation == BackendOperation::Tokenize;
        const bool json = op.kind == "http_json" && delivery == OperationDelivery::Buffered && json_operation && transport.value("response_codec", "") == "json.v1" && !transport.contains("stream_codec");
        const bool sse = op.kind == "http_sse" && delivery == OperationDelivery::Streaming && operation <= BackendOperation::Responses && transport.value("stream_codec", "") == "openai_sse.v1" && !transport.contains("response_codec");
        require((json || sse) && op.method == (operation == BackendOperation::Slots ? "GET" : "POST"), "unsupported_execution_feature", p + "/transport");
        require(ports.count(op.port) && op.port == launch["readiness"]["port"], "invalid_reference", p + "/transport/port");
        require(!op.path.empty() && op.path[0] == '/' && op.path.find("://") == std::string::npos && op.path.find_first_of("\r\n") == std::string::npos && op.path.find('\0') == std::string::npos, "invalid_endpoint", p);
        op.codec = transport.value("response_codec", transport.value("stream_codec", ""));
        op.timeout_seconds = transport["timeout"] == "global" ? 0 : transport["timeout"].get<std::uint64_t>();
        const auto& request = record["request"];
        op.identity = request["base"] == "identity";
        op.omit_missing = request["omit_missing"];
        op.remove = request["remove"].get<std::vector<std::string>>();
        for (const auto& destination : op.remove) pointer(destination, p + "/request/remove");
        for (const auto& [destination, value] : request["set"].items()) {
            budget.work(destination.size());
            pointer(destination, p + "/request/set");
            for (const auto& [prior, expression] : op.set) { budget.work(prior.size() + destination.size()); require(!prefix(prior, destination), "mapping_overlap", p + "/request/set"); }
            budget.keep(destination.size() * 2 + p.size() + 128);
            op.set.emplace(destination, p + "/request/set/" + v2::pointer_token(destination));
        }
        program.descriptions.push_back(op.description);
        program.operations.push_back(std::move(op));
    }
}

}
