#include "backend_definition_resources.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/configured/backend_catalog.h"
#include "lemon/recipe_options.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace lemon::backends {
namespace {
std::unique_ptr<const BackendCatalog> catalog;
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

json descriptor_row(const BackendDescriptor& descriptor, const BackendDefinition* definition, bool external) {
    json options = json::array();
    const auto common = RecipeOptions::common_option_defaults();
    if (descriptor.uses_ctx_size) options.push_back(legacy_option("ctx_size", common["ctx_size"]));
    if (definition) for (const auto& option : definition->records["metadata"]["options"]) options.push_back(public_option(option));
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
    json row = {{"recipe", descriptor.recipe}, {"definition_id", definition ? json(definition->id) : json()},
        {"source", external ? "external" : "builtin"}, {"display_name", descriptor.display_name},
        {"supported_modes", descriptor.supported_modes}, {"options", options}, {"operations_complete", definition != nullptr},
        {"operations", json::array()}, {"support", support}};
    if (external) {
        const auto& backend = definition->records["launch"]["backend"];
        row["backend_option"] = backend.contains("option") ? backend["option"] : json();
    }
    row["definition_hash"] = definition ? definition->digest : schema_digest(row.dump());
    return row;
}
}

BackendCatalog::BackendCatalog(const std::vector<DefinitionSource>& sources)
    : data_(std::make_shared<BackendCatalogData>()), registrations_(builtin_registrations()), sources_(sources) {
    std::set<std::string> ids, recipes, sections{"no_broadcast"}, option_names{"no_broadcast"};
    const auto defaults = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/defaults.json"));
    for (const auto& [key, value] : defaults.items()) { sections.insert(key); option_names.insert(key); }
    for (const auto& [key, value] : RecipeOptions::common_option_defaults().items()) option_names.insert(key);
    for (const auto* text : {k_thinksound_definition, k_ds4_definition, k_ryzenai_definition}) {
        auto result = BackendDefinitionStore().validate({"embedded", text, DefinitionOrigin::Builtin});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        ids.insert(result.definition->id);
    }
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
        auto row = descriptor_row(*descriptor, definition, false);
        if (definition) {
            const auto trusted = descriptor->recipe == "thinksound" ? TrustedCompatibility::LegacyNonChatSse : TrustedCompatibility::None;
            for (const auto& binding : configured_operation_bindings(*definition, trusted)) row["operations"].push_back(operation_row(binding.description));
        }
        schema_["recipes"].push_back(std::move(row));
    }
    for (const auto& source : sources) {
        auto result = BackendDefinitionStore().validate({source.file, source.text, DefinitionOrigin::External});
        if (!result.definition) throw DefinitionValidationError(std::move(result.diagnostics));
        const auto& definition = *result.definition;
        if (!ids.insert(definition.id).second) fail("id_collision", source.file, definition.id, "/id", "Definition ID already owned");
        if (!recipes.insert(definition.recipe).second) fail("recipe_collision", source.file, definition.id, "/recipe", "Recipe already owned");
        std::shared_ptr<const ConfiguredBackendEntry> entry;
        try { entry = ConfiguredBackendEntry::make(result.definition); }
        catch (const std::exception&) { fail("invalid_binding", source.file, definition.id, "", "Definition cannot bind to installed server primitives"); }
        if (!sections.insert(entry->descriptor.effective_config_section()).second) fail("section_collision", source.file, definition.id, "/metadata/config_section", "Configuration section already owned");
        auto& contracts = data_->external_options[definition.recipe];
        contracts = json::object();
        for (size_t i = 0; i < definition.records["metadata"]["options"].size(); ++i) {
            const auto& option = definition.records["metadata"]["options"][i];
            const std::string name = option["name"];
            if (!option_names.insert(name).second) fail("option_collision", source.file, definition.id, "/metadata/options/" + std::to_string(i) + "/name", "Option already owned");
            contracts[name] = public_option(option);
        }
        const auto& install = definition.records["install"];
        if (install["strategy"] == "github_release") {
            std::set<std::string> backends;
            const auto& selection = definition.records["launch"]["backend"];
            if (selection.contains("fixed")) backends.insert(selection["fixed"]);
            for (const auto& row : install["cases"]) for (const auto& backend : row.value("backend", json::array())) backends.insert(backend);
            const auto versions = install.value("default_versions", json::object());
            const auto pins = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/backend_versions.json"));
            const auto shipped = pins.value(entry->spec.pin_recipe, json::object());
            std::set<std::string> normalized;
            for (const auto& backend : backends) {
                if (backend == "rocm" && !entry->descriptor.rocm_channels.empty()) {
                    for (const auto& channel : entry->descriptor.rocm_channels) normalized.insert("rocm-" + channel);
                } else if (backend != "system") normalized.insert(backend);
            }
            for (const auto& backend : normalized) if (!shipped.contains(backend) && (!versions.contains(backend) || versions[backend].get<std::string>().empty())) fail("required", source.file, definition.id, "/install/default_versions", "Managed backend requires a default version");
        }
        auto row = descriptor_row(entry->descriptor, &definition, true);
        for (const auto& binding : entry->bindings) row["operations"].push_back(operation_row(binding.description));
        schema_["recipes"].push_back(std::move(row));
        data_->descriptors.push_back(&entry->descriptor);
        registrations_.push_back({&entry->descriptor, entry->factory, &entry->spec, entry->ops.get()});
        entries_.push_back(std::move(entry));
    }
    schema_["catalog_id"] = schema_digest(json{{"export", schema_}, {"arms", arms}}.dump());
}

json BackendCatalog::export_schema(const std::string& recipe) const {
    json result = schema_;
    if (recipe.empty()) return result;
    result["recipes"] = json::array();
    for (const auto& row : schema_["recipes"]) if (row["recipe"] == recipe) result["recipes"].push_back(row);
    return result;
}

json BackendCatalog::validate(const std::string& bytes) const {
    json response = {{"valid", false}, {"validation", "server"}, {"catalog_id", schema_["catalog_id"]}, {"diagnostics", json::array()}};
    auto parsed = BackendDefinitionStore().validate({"", bytes, DefinitionOrigin::External});
    auto errors = parsed.diagnostics;
    if (parsed.definition) {
        auto sources = sources_;
        sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const auto& source) {
            const auto result = BackendDefinitionStore().validate(source);
            return result.definition && result.definition->id == parsed.definition->id && result.definition->recipe == parsed.definition->recipe;
        }), sources.end());
        sources.push_back({"", bytes, DefinitionOrigin::External});
        try {
            BackendCatalog candidate(sources);
            response["valid"] = true;
            response["definition_id"] = parsed.definition->id;
            response["recipe"] = parsed.definition->recipe;
            response["definition_hash"] = parsed.definition->digest;
        } catch (const DefinitionValidationError& error) { errors = error.diagnostics; }
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
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.path().extension() != ".json") continue;
            const auto status = entry.symlink_status();
            if (fs::is_directory(status)) continue;
            paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) { return left.filename().u8string() < right.filename().u8string(); });
        for (const auto& path : paths) {
            if (!fs::is_regular_file(fs::symlink_status(path))) fail("invalid_file", utils::path_to_utf8(path), "", "", "Selected JSON entry must be a regular file");
            if (!readable(fs::status(path).permissions())) fail("unreadable_file", utils::path_to_utf8(path), "", "", "Selected definition is unreadable");
            std::ifstream input(path, std::ios::binary);
            if (!input) fail("unreadable_file", utils::path_to_utf8(path), "", "", "Selected definition cannot be opened");
            std::string text(1024 * 1024 + 1, '\0');
            input.read(text.data(), static_cast<std::streamsize>(text.size()));
            text.resize(static_cast<size_t>(input.gcount()));
            if (input.bad()) fail("unreadable_file", utils::path_to_utf8(path), "", "", "Selected definition cannot be read");
            if (text.size() > 1024 * 1024) fail("limit_exceeded", utils::path_to_utf8(path), "", "", "Definition exceeds 1 MiB");
            result.push_back({utils::path_to_utf8(path), std::move(text), DefinitionOrigin::External});
        }
    } catch (const DefinitionValidationError&) { throw; }
    catch (const std::exception&) { fail("discovery_failed", utils::path_to_utf8(directory), "", "", "Cannot enumerate backend definitions"); }
    return result;
}

void bootstrap_backend_catalog(const std::string& config_dir, bool builtin_only) {
    const auto directory = utils::path_to_utf8(utils::path_from_utf8(config_dir).lexically_normal());
    if (catalog) {
        if (directory != frozen_directory || builtin_only != frozen_builtin_only) throw std::logic_error("Backend catalog is already frozen");
        return;
    }
    if (descriptor_snapshot_consumed()) throw std::logic_error("Backend descriptors were consumed before catalog bootstrap");
    std::unique_ptr<BackendCatalog> candidate;
    try { candidate = std::make_unique<BackendCatalog>(builtin_only ? std::vector<DefinitionSource>{} : discover_backend_definitions(config_dir)); }
    catch (const DefinitionValidationError& error) {
        std::string diagnostic;
        for (const auto& row : error.diagnostics) diagnostic += row.file + " " + row.definition + " " + row.pointer + " " + row.code + ": " + row.message + "\n";
        throw std::runtime_error(diagnostic);
    }
    frozen_directory = directory;
    frozen_builtin_only = builtin_only;
    auto data = candidate->data();
    catalog = std::move(candidate);
    publish_backend_catalog_data(std::move(data));
}
const BackendCatalog* backend_catalog() { return catalog.get(); }
const BackendCatalog& frozen_backend_catalog() {
    if (catalog) return *catalog;
    all_descriptors();
    static const BackendCatalog builtins({});
    return builtins;
}
std::string external_default_version(const std::string& pin_recipe, const std::string& backend, const json& shipped_pins) {
    for (const auto& registration : all_registrations()) {
        const auto* spec = registration.spec;
        if (!spec || (spec->pin_recipe != pin_recipe && registration.descriptor->recipe != pin_recipe) || !spec->definition || spec->definition->origin != DefinitionOrigin::External) continue;
        const auto pinned = shipped_pins.value(spec->pin_recipe, json::object());
        if (pinned.contains(backend) && pinned[backend].is_string()) return pinned[backend].get<std::string>();
        const auto versions = spec->definition->records["install"].value("default_versions", json::object());
        if (versions.contains(backend)) return versions[backend].get<std::string>();
    }
    return "";
}

}
