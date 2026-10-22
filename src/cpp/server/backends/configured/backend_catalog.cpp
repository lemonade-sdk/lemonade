#include "../backend_builtin_seeds_internal.h"
#include "../backend_catalog_publication_internal.h"
#include "backend_catalog_admission_internal.h"
#include "backend_catalog_ledger_internal.h"
#include "backend_definition_resources.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/configured/backend_catalog.h"
#include "lemon/backends/configured/backend_spec_utils.h"
#include "lemon/recipe_options.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/wrapped_server.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

namespace lemon::backends {
namespace {
std::string frozen_directory;
bool frozen_builtin_only = false;

void fail(const std::string& code, const std::string& file, const std::string& id, const std::string& pointer, const std::string& message) {
    throw DefinitionValidationError({{code, id, file, pointer, message}});
}

json public_option(const json& option) {
    json row = json::object();
    for (const auto* key : {"name", "value_type", "default_value", "minimum", "maximum", "enum", "help", "group", "ui"}) {
        if (option.contains(key)) row[key] = option[key];
    }
    row["unset_values"] = option.value("unset_values", json::array({nullptr}));
    return row;
}

json legacy_option(const std::string& name, const json& value, const std::string& help = "", const std::string& group = "") {
    const std::string type = value.is_boolean() || name == "auto_evict" || name == "auto_update" ? "boolean" :
        value.is_number_integer() ? "integer" : value.is_number() ? "number" : "string";
    json row = {{"name", name}, {"value_type", type}, {"default_value", value},
        {"unset_values", name == "ctx_size" ? json::array({nullptr}) : json::array({nullptr, -1, "", "auto"})}};
    if (!help.empty()) row["help"] = help;
    if (!group.empty()) row["group"] = group;
    return row;
}

json operation_row(const OperationDescription& description) {
    static const char* modes[] = {"chat", "embedding", "reranking", "transcription", "image", "tts", "audio-generation", "classification", "mesh"};
    json selected = json::array();
    for (size_t i = 0; i < k_operation_model_types.size(); ++i) if (description.model_types & (1u << i)) selected.push_back(modes[i]);
    return {{"operation", std::string(backend_operation_name(description.operation))},
        {"delivery", description.delivery == OperationDelivery::Buffered ? "buffered" : description.delivery == OperationDelivery::Streaming ? "streaming" : "realtime"},
        {"model_modes", selected}, {"provenance", description.provenance == OperationProvenance::Implemented ? "implemented" : "legacy_forwarding"}};
}

json descriptor_row(const BackendDescriptor& descriptor, const BackendDefinition* definition, bool external, const ExecutableDefinition* executable = nullptr) {
    const json* records = definition ? &definition->records : executable ? &executable->compiled().normalized_declaration() : nullptr;
    json options = json::array();
    const auto common = RecipeOptions::common_option_defaults();
    if (descriptor.uses_ctx_size) options.push_back(legacy_option("ctx_size", common["ctx_size"]));
    if (records) for (const auto& option : records->at("metadata").at("options")) options.push_back(public_option(option));
    else for (const auto& option : descriptor.options) options.push_back(legacy_option(option.name, option.default_value, option.help, option.group));
    for (const auto* key : {"merge_args", "auto_evict", "evict_idle_timeout", "downsize_idle_timeout", "evict_weight_factor", "pinned", "auto_update"}) options.push_back(legacy_option(key, common[key]));
    json support = json::array();
    for (const auto& row : descriptor.support) {
        json gates = json::object();
        for (const auto& [arch, gate] : row.arch_gates) gates[arch] = {{"os", gate.os}, {"channels", gate.channels}};
        support.push_back({{"backend", row.backend},
        {"tier", row.tier == BackendTier::Core ? "core" : row.tier == BackendTier::Experimental ? "experimental" : "guest"},
        {"format", row.format == BackendFormat::Native ? "native" : row.format == BackendFormat::Python ? "python" : "container"},
        {"supported_os", row.supported_os}, {"devices", row.devices}, {"device_summary", row.device_summary}, {"arch_gates", gates}});
    }
    json row = {{"recipe", descriptor.recipe}, {"definition_id", definition ? json(definition->id) : executable ? json(executable->compiled().authorized().definition().key.id) : json()},
        {"source", external ? "external" : "builtin"}, {"display_name", descriptor.display_name},
        {"supported_modes", descriptor.supported_modes}, {"options", options}, {"operations_complete", records != nullptr},
        {"operations", json::array()}, {"support", support}};
    if (external) {
        const auto& backend = records->at("launch").at("backend");
        row["backend_option"] = backend.contains("option") ? backend["option"] : json();
    }
    row["definition_hash"] = definition ? definition->digest : executable ? executable->compiled().authorized().definition().definition_hash : schema_digest(row.dump());
    return row;
}
}

BackendCatalog::BackendCatalog(const std::vector<DefinitionSource>& sources) {
    std::vector<ExternalDefinitionInput> inputs;
    if (sources.size() > DefinitionLimits{}.snapshot_documents) fail("limit_exceeded", "", "", "", "Catalog source count exceeded");
    for (const auto& source : sources) inputs.push_back({source.text, source.file});
    auto result = build_backend_catalog(freeze_builtin_catalog(), inputs);
    if (!result.catalog) {
        std::vector<DefinitionDiagnostic> errors;
        for (const auto& row : result.diagnostics.entries) errors.push_back({row.code, row.definition, "", row.pointer, row.message});
        throw DefinitionValidationError(std::move(errors));
    }
    auto completed = std::const_pointer_cast<BackendCatalog>(result.catalog);
    *this = std::move(*completed);
}

void BackendCatalog::finalize() {
    data_ = std::make_shared<BackendCatalogData>();
    const auto& seeds = builtins_->seeds_ ? *builtins_->seeds_ : builtin_registration_seeds();
    for (const auto& seed : seeds) {
        if (seed.definition_id.empty()) { registrations_.push_back(seed.legacy); continue; }
        const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
            if (!entry->executable_definition) return false;
            const auto& key = entry->executable_definition->compiled().authorized().definition().key;
            return key.space == DefinitionNamespace::Builtin && key.id == seed.definition_id;
        });
        if (found == entries_.end() || &(*found)->published_descriptor() != seed.descriptor)
            throw std::logic_error("Builtin seed has no finalized canonical entry");
        const auto& entry = *found;
        registrations_.push_back({&entry->published_descriptor(), entry->factory, &entry->spec, entry->backend_ops(), entry.get()});
    }
    for (const auto& entry : entries_) {
        if (!entry->executable_definition || entry->executable_definition->compiled().authorized().definition().key.space != DefinitionNamespace::Builtin) continue;
        const auto found = std::find_if(registrations_.begin(), registrations_.end(), [&](const auto& row) { return row.configured_entry == entry.get(); });
        if (found == registrations_.end()) throw std::logic_error("Unregistered builtin executable entry");
    }
    std::set<std::string> ids, recipes, sections{"no_broadcast"}, option_names{"no_broadcast"};
    const auto& defaults = builtins_->defaults_;
    for (const auto& [key, value] : defaults.items()) { sections.insert(key); option_names.insert(key); }
    for (const auto& [key, value] : RecipeOptions::common_option_defaults().items()) option_names.insert(key);
    json arms = json::array();
    schema_ = {{"schema_version", 1}, {"control_version", 1}, {"definition_schema_version", 1},
        {"feature_manifest_id", schema_digest(installed_definition_features().dump())}, {"recipes", json::array()}};
    for (const auto& registration : registrations_) {
        const auto* descriptor = registration.descriptor;
        data_->descriptors.push_back(descriptor);
        recipes.insert(descriptor->recipe);
        sections.insert(descriptor->effective_config_section());
        for (const auto& option : descriptor->options) option_names.insert(option.name);
        const auto* definition = registration.spec && registration.spec->definition ? registration.spec->definition.get() : nullptr;
        arms.push_back({descriptor->recipe, definition ? "D" : "A"});
        const auto* entry = registration.configured_entry;
        const auto* executable = entry ? entry->executable_definition.get() : nullptr;
        auto row = descriptor_row(*descriptor, definition, false, executable);
        if (definition) {
            ids.insert(definition->id);
            if (!entry) throw std::logic_error("Configured registration has no finalized entry");
        }
        if (entry) {
            for (const auto& binding : entry->bindings) row["operations"].push_back(operation_row(binding.description));
        }
        schema_["recipes"].push_back(std::move(row));
    }
    json executions = json::array();
    for (const auto& entry : entries_) {
        const bool builtin = entry->executable_definition && entry->executable_definition->compiled().authorized().definition().key.space == DefinitionNamespace::Builtin;
        if (entry->executable_definition) executions.push_back(entry->executable_definition->execution_id());
        if (builtin) continue;
        const auto& records = entry->records();
        const auto id = entry->definition ? entry->definition->id : entry->executable_definition->compiled().authorized().definition().key.id;
        const auto& recipe = entry->descriptor.recipe;
        const std::string file;
        if (!ids.insert(id).second) fail("id_collision", file, id, "/id", "Definition ID already owned");
        if (!recipes.insert(recipe).second) fail("recipe_collision", file, id, "/recipe", "Recipe already owned");
        if (!sections.insert(entry->descriptor.effective_config_section()).second) fail("section_collision", file, id, "/metadata/config_section", "Configuration section already owned");
        auto& contracts = data_->external_options[recipe];
        contracts = json::object();
        for (size_t i = 0; i < records["metadata"]["options"].size(); ++i) {
            const auto& option = records["metadata"]["options"][i];
            const std::string name = option["name"];
            if (!option_names.insert(name).second) fail("option_collision", file, id, "/metadata/options/" + std::to_string(i) + "/name", "Option already owned");
            contracts[name] = public_option(option);
        }
        const auto& install = records["install"];
        if (install["strategy"] == "github_release") {
            std::set<std::string> backends;
            const auto& selection = records["launch"]["backend"];
            if (selection.contains("fixed") && selection["fixed"].is_string()) backends.insert(selection["fixed"]);
            else if (selection.contains("fixed")) for (const auto& [os, fixed] : selection["fixed"].items()) backends.insert(fixed);
            for (const auto& row : install["cases"]) for (const auto& backend : row.value("backend", json::array())) backends.insert(backend);
            const auto versions = install.value("default_versions", json::object());
            const auto& pins = builtins_->pins_;
            const auto shipped = pins.value(entry->spec.effective_pin_recipe(), json::object());
            std::set<std::string> normalized;
            for (const auto& backend : backends) {
                if (backend == "rocm" && !entry->descriptor.rocm_channels.empty()) {
                    for (const auto& channel : entry->descriptor.rocm_channels) normalized.insert("rocm-" + channel);
                } else if (backend != "system") normalized.insert(backend);
            }
            for (const auto& backend : normalized) if (!shipped.contains(backend) && (!versions.contains(backend) || versions[backend].get<std::string>().empty())) fail("required", file, id, "/install/default_versions", "Managed backend requires a default version");
        }
        auto row = descriptor_row(entry->descriptor, entry->definition.get(), true, entry->executable_definition.get());
        for (const auto& binding : entry->bindings) row["operations"].push_back(operation_row(binding.description));
        schema_["recipes"].push_back(std::move(row));
        data_->descriptors.push_back(&entry->descriptor);
        registrations_.push_back({&entry->descriptor, entry->factory, &entry->spec, entry->backend_ops(), entry.get()});
    }
    json identity = {{"export", schema_}, {"arms", arms}};
    if (!executions.empty()) { identity["executions"] = executions; identity["runner_service_policy"] = 3; }
    schema_["catalog_id"] = schema_digest(identity.dump());
}

json BackendCatalog::export_schema(const std::string& recipe, unsigned representation) const {
    if (representation != 1 && representation != 2) throw std::invalid_argument("Unknown catalog representation");
    json result = schema_;
    if (representation == 2) {
        result["schema_version"] = 2;
        result["definition_schema_version"] = 2;
        result["accepted_definition_versions"] = {1, 2};
        result["acceptance_stage"] = "execution_finalization";
        result["executable_definition_versions"] = {1, 2};
        result["resolver_revision"] = 1;
        result["authorizer_revision"] = 1;
        result["grant_policy_revision"] = 1;
        result["legacy_feature_manifest_id"] = schema_["feature_manifest_id"];
        result["admission_manifest"] = installed_definition_admission_features();
        result["feature_manifest_id"] = schema_digest(result["admission_manifest"].dump());
        result["export_registry_id"] = installed_builtin_exports().digest();
        result["catalog_v2_id"] = schema_digest(json{{"domain", "lemonade.backend-catalog-v2.v1"},
            {"admission", admitted_->admission_id()}, {"executable", schema_["catalog_id"]}}.dump());
        for (auto& row : result["recipes"]) {
            row["input_schema_version"] = row["definition_id"].is_null() ? json() : json(1);
            for (const auto& entry : entries_) if (entry->executable_definition && entry->descriptor.recipe == row["recipe"]) row["input_schema_version"] = 2;
        }
    }
    if (recipe.empty()) return result;
    auto rows = std::move(result["recipes"]);
    result["recipes"] = json::array();
    for (const auto& row : rows) if (row["recipe"] == recipe) result["recipes"].push_back(row);
    return result;
}

json BackendCatalog::validate(const std::string& bytes) const {
    json response = {{"valid", false}, {"validation", "server"}, {"catalog_id", schema_["catalog_id"]}, {"diagnostics", json::array()}};
    CatalogInputHeader header;
    try { header = inspect_catalog_input(bytes, DefinitionLimits{}); } catch (...) {}
    const bool v2_input = header.version == 2;
    auto parsed = v2_input ? DefinitionValidationResult{} : BackendDefinitionStore().validate({"", bytes, DefinitionOrigin::External});
    auto errors = parsed.diagnostics;
    if (v2_input) {
        response["input_schema_version"] = 2;
        response["stages"] = {{"resolved", false}, {"authorized", false}, {"catalog_admitted", false}, {"executable", false}};
    }
    if (v2_input || parsed.definition) {
        std::vector<ExternalDefinitionInput> inputs;
        for (const auto& source : admitted_->sources().sources_) {
            if (source.header.id == header.id && source.header.recipe == header.recipe) continue;
            inputs.push_back({source.bytes, ""});
        }
        inputs.push_back({bytes, ""});
        const auto candidate = build_backend_catalog(builtins_, inputs);
        if (v2_input) {
            const auto& stages = candidate.stages;
            response["stages"]["resolved"] = stages.resolved;
            response["stages"]["authorized"] = stages.authorized;
            response["stages"]["catalog_admitted"] = stages.catalog_admitted;
            if (stages.catalog_admitted) {
                response["stages"]["compiled"] = stages.compiled;
                response["stages"]["bound"] = stages.bound;
            }
            response["stages"]["executable"] = stages.executable;
            for (const auto& error : candidate.diagnostics.entries) response["diagnostics"].push_back(public_admission_diagnostic(error));
            if (candidate.catalog) {
                const auto authorized = candidate.catalog->admitted_->v2_definition(header.id);
                const auto& resolved = authorized->definition();
                response["valid"] = true;
                response["definition_id"] = header.id;
                response["recipe"] = header.recipe;
                response["definition_hash"] = resolved.definition_hash;
                response["dependency_digest"] = resolved.dependency_digest;
                response["candidate_admission_id"] = candidate.catalog->admitted_->admission_id();
            }
        } else if (candidate.catalog) {
            response["valid"] = true;
            response["definition_id"] = parsed.definition->id;
            response["recipe"] = parsed.definition->recipe;
            response["definition_hash"] = parsed.definition->digest;
        } else {
            errors.clear();
            for (const auto& row : candidate.diagnostics.entries) errors.push_back({row.code, row.definition, "", row.pointer, row.message});
        }
    }
    for (const auto& error : errors) response["diagnostics"].push_back({{"code", error.code}, {"definition", error.definition}, {"pointer", error.pointer}, {"message", error.message}});
    return response;
}

std::vector<DefinitionSource> discover_backend_definitions(const std::string& config_dir) {
    namespace fs = std::filesystem;
    const auto directory = utils::path_from_utf8(config_dir) / "backends.d";
    std::vector<DefinitionSource> result;
    try {
        if (!fs::exists(directory)) return result;
        if (!fs::is_directory(directory)) throw std::runtime_error("Not a directory");
        const auto readable = [](fs::perms permissions) { return (permissions & (fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read)) != fs::perms::none; };
        if (!readable(fs::status(directory).permissions())) throw std::runtime_error("Directory unreadable");
        const DefinitionLimits limits;
        std::size_t count = k_backend_definition_source_count, bytes = k_backend_definition_source_bytes;
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.path().extension() != ".json") continue;
            const auto status = entry.symlink_status();
            if (fs::is_directory(status)) continue;
            if (count >= limits.snapshot_documents) fail("limit_exceeded", "", "", "", "Catalog source count exceeded");
            ++count;
            paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) { return left.filename().u8string() < right.filename().u8string(); });
        for (const auto& path : paths) {
            if (!fs::is_regular_file(fs::symlink_status(path))) fail("invalid_file", utils::path_to_utf8(path), "", "", "Selected JSON entry must be a regular file");
            if (!readable(fs::status(path).permissions())) fail("unreadable_file", utils::path_to_utf8(path), "", "", "Selected definition is unreadable");
            const auto size = fs::file_size(path);
            if (size > limits.source_bytes || size > limits.snapshot_source_bytes - bytes) fail("limit_exceeded", "", "", "", "Catalog source bytes exceeded");
            const auto allowance = std::min(limits.source_bytes, limits.snapshot_source_bytes - bytes);
            std::ifstream input(path, std::ios::binary);
            if (!input) fail("unreadable_file", utils::path_to_utf8(path), "", "", "Selected definition cannot be opened");
            std::string text(allowance + 1, '\0');
            input.read(text.data(), static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<size_t>(input.gcount()));
            if (input.bad()) fail("unreadable_file", utils::path_to_utf8(path), "", "", "Selected definition cannot be read");
            if (text.size() > allowance) fail("limit_exceeded", utils::path_to_utf8(path), "", "", "Definition exceeds 1 MiB");
            bytes += text.size();
            result.push_back({utils::path_to_utf8(path), std::move(text), DefinitionOrigin::External});
        }
    } catch (const DefinitionValidationError&) { throw; }
    catch (const std::exception&) { fail("discovery_failed", utils::path_to_utf8(directory), "", "", "Cannot enumerate backend definitions"); }
    return result;
}

void bootstrap_backend_catalog(const std::string& config_dir, bool builtin_only) {
    const auto directory = utils::path_to_utf8(utils::path_from_utf8(config_dir).lexically_normal());
    if (backend_catalog()) {
        if (directory != frozen_directory || builtin_only != frozen_builtin_only) throw std::logic_error("Backend catalog is already frozen");
        return;
    }
    CatalogPublicationGuard publication(catalog_publication());
    std::shared_ptr<BackendCatalog> candidate;
    try { candidate = std::make_shared<BackendCatalog>(builtin_only ? std::vector<DefinitionSource>{} : discover_backend_definitions(config_dir)); }
    catch (const DefinitionValidationError& error) {
        std::string diagnostic;
        for (const auto& row : error.diagnostics) diagnostic += row.file + " " + row.definition + " " + row.pointer + " " + row.code + ": " + row.message + "\n";
        throw std::runtime_error(diagnostic);
    }
    frozen_directory = directory;
    frozen_builtin_only = builtin_only;
    auto data = candidate->data();
    publication.commit(std::move(data), std::move(candidate));
}
const BackendCatalog* backend_catalog() { return static_cast<const BackendCatalog*>(catalog_publication().server()); }
const BackendCatalog& frozen_backend_catalog() {
    if (const auto* catalog = static_cast<const BackendCatalog*>(catalog_publication().consume_server())) return *catalog;
    static const BackendCatalog builtins({});
    return builtins;
}
std::string external_default_version(const std::string& pin_recipe, const std::string& backend, const json& shipped_pins) {
    for (const auto& registration : all_registrations()) {
        const auto* spec = registration.spec;
        if (spec && spec->executable_definition && spec->pin_recipe == pin_recipe) {
            return configured_external_default_version(*spec, backend, shipped_pins);
        }
        if (!spec || (spec->pin_recipe != pin_recipe && registration.descriptor->recipe != pin_recipe) || !spec->definition || spec->definition->origin != DefinitionOrigin::External) continue;
        const auto pinned = shipped_pins.value(spec->pin_recipe, json::object());
        if (pinned.contains(backend) && pinned[backend].is_string()) return pinned[backend].get<std::string>();
        const auto versions = spec->definition->records["install"].value("default_versions", json::object());
        if (versions.contains(backend)) return versions[backend].get<std::string>();
    }
    return "";
}

}
