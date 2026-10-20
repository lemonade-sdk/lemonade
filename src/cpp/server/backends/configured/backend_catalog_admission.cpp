#include "backend_catalog_admission_internal.h"
#include "backend_definition_resources.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/backend_option_contract.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lemon::backends {
namespace {
using namespace v2;

struct IntakeFailure {
    std::string code;
};

class HeaderReader : public nlohmann::json_sax<Json> {
public:
    explicit HeaderReader(const DefinitionLimits& bounds) : limits(bounds) {}
    CatalogInputHeader header;
    bool null() override { return scalar(nullptr); }
    bool boolean(bool value) override { return scalar(value); }
    bool number_integer(number_integer_t value) override { return scalar(value); }
    bool number_unsigned(number_unsigned_t value) override { return scalar(value); }
    bool number_float(number_float_t value, const string_t&) override { return scalar(value); }
    bool string(string_t& value) override { return scalar(value); }
    bool binary(binary_t&) override { return false; }
    bool start_object(std::size_t) override { return start(true); }
    bool start_array(std::size_t) override { return start(false); }
    bool end_object() override { stack.pop_back(); return true; }
    bool end_array() override { stack.pop_back(); return true; }
    bool key(string_t& value) override {
        auto& frame = stack.back();
        if (!frame.keys.insert(value).second) throw IntakeFailure{"duplicate_key"};
        frame.key = value;
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
private:
    struct Frame { bool object; std::string key; std::set<std::string> keys; bool parent; };
    const DefinitionLimits& limits;
    std::vector<Frame> stack;
    void count() {
        if (++header.nodes > limits.closure_source_nodes) throw LimitExceeded();
    }
    bool start(bool object) {
        count();
        if (stack.size() >= limits.source_depth) throw LimitExceeded();
        const bool parent = stack.size() == 1 && stack[0].key == "extends";
        if (parent) header.parent = object ? Json::object() : Json::array();
        stack.push_back({object, "", {}, parent});
        return true;
    }
    bool scalar(const Json& value) {
        count();
        if (stack.size() == 1 && stack[0].object) {
            const auto& key = stack[0].key;
            if (key == "schema_version" && value.is_number_integer() && value >= 0 && value <= std::numeric_limits<unsigned>::max()) header.version = value.get<unsigned>();
            if (key == "id" && value.is_string()) header.id = value.get<std::string>();
            if (key == "recipe" && value.is_string()) header.recipe = value.get<std::string>();
        } else if (stack.size() == 2 && stack.back().parent && stack.back().object) header.parent[stack.back().key] = value;
        return true;
    }
};

void keep(std::size_t& retained, std::size_t bytes, const DefinitionLimits& limits) {
    charge(retained, bytes, limits.retained_logical_bytes);
}

void error(CompositionDiagnostics& errors, const DefinitionLimits& limits, const std::string& code,
    const std::string& id = "", const std::string& pointer = "") {
    if (errors.entries.size() < limits.diagnostics) errors.entries.push_back({code, id, pointer, "Catalog admission failed", {}, {}, {}});
    else errors.truncated = true;
}

void append(CompositionDiagnostics& errors, const CompositionDiagnostics& extra, const DefinitionLimits& limits) {
    std::size_t used = errors.entries.size();
    for (const auto& row : errors.entries) used = checked_sum(used, row.related.size());
    for (const auto& row : extra.entries) {
        if (used >= limits.diagnostics) { errors.truncated = true; break; }
        ++used;
        auto copy = row;
        copy.related.clear();
        for (const auto& location : row.related) {
            if (used >= limits.diagnostics) { errors.truncated = true; break; }
            ++used;
            copy.related.push_back(location);
        }
        errors.entries.push_back(std::move(copy));
    }
    errors.truncated |= extra.truncated;
}

std::size_t logical_json(const Json& value) {
    std::size_t size = sizeof(Json);
    if (value.is_string()) size = checked_sum(size, value.get_ref<const std::string&>().size());
    if (value.is_object()) for (const auto& [key, field] : value.items()) size = checked_sum(size, checked_sum(key.size(), logical_json(field)));
    else if (value.is_array()) for (const auto& field : value) size = checked_sum(size, logical_json(field));
    return size;
}

std::size_t v1_allowance(std::size_t bytes, std::size_t nodes) {
    // The v1 compiler retains expressions and operation programs in addition to its DOM.
    return checked_sum(checked_product(bytes, 8), checked_product(nodes, 512));
}

void reserve(std::set<std::string>& values, const std::string& value, std::size_t& retained, const DefinitionLimits& limits) {
    keep(retained, checked_sum(value.size(), sizeof(std::string) + 64), limits);
    values.insert(value);
}

}

CatalogInputHeader inspect_catalog_input(std::string_view bytes, const DefinitionLimits& limits) {
    if (bytes.size() > limits.source_bytes) throw LimitExceeded();
    HeaderReader reader(limits);
    if (!Json::sax_parse(bytes.begin(), bytes.end(), &reader)) throw IntakeFailure{"syntax_error"};
    return std::move(reader.header);
}

std::shared_ptr<const FrozenBuiltinCatalog> CatalogAdmissionBuilder::freeze() {
    auto frozen = std::shared_ptr<FrozenBuiltinCatalog>(new FrozenBuiltinCatalog());
    const DefinitionLimits limits;
    auto& retained = frozen->logical_bytes_;
    keep(retained, sizeof(FrozenBuiltinCatalog) + 1024, limits);
    frozen->defaults_ = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/defaults.json"));
    frozen->pins_ = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/backend_versions.json"));
    keep(retained, checked_sum(logical_json(frozen->defaults_), logical_json(frozen->pins_)), limits);
    for (const auto* key : {"no_broadcast"}) { reserve(frozen->sections_, key, retained, limits); reserve(frozen->options_, key, retained, limits); }
    for (const auto& [key, value] : frozen->defaults_.items()) { reserve(frozen->sections_, key, retained, limits); reserve(frozen->options_, key, retained, limits); }
    for (const auto& [key, value] : RecipeOptions::common_option_defaults().items()) reserve(frozen->options_, key, retained, limits);
    for (const auto& registration : builtin_registrations()) {
        const auto& descriptor = *registration.descriptor;
        reserve(frozen->ids_, descriptor.recipe, retained, limits);
        reserve(frozen->ids_, descriptor.effective_config_section(), retained, limits);
        reserve(frozen->recipes_, descriptor.recipe, retained, limits);
        reserve(frozen->sections_, descriptor.effective_config_section(), retained, limits);
        for (const auto& option : descriptor.options) {
            reserve(frozen->options_, option.name, retained, limits);
            reserve(frozen->persisted_[descriptor.effective_config_section()], backend_option_config_key(option.name), retained, limits);
        }
        if (registration.spec) for (const auto& alias : {registration.spec->recipe, registration.spec->pin_recipe})
            if (!alias.empty()) reserve(frozen->ids_, alias, retained, limits);
        if (registration.spec && registration.spec->definition) {
            const auto& definition = *registration.spec->definition;
            reserve(frozen->ids_, definition.id, retained, limits);

            keep(retained, checked_product(logical_json(definition.records), 32), limits);
        }
    }
    frozen->definitions_ = k_backend_definition_source_count;
    frozen->source_bytes_ = k_backend_definition_source_bytes;
    frozen->authority_ = load_v2_authority_snapshot({}).snapshot;
    if (!frozen->authority_) throw std::logic_error("Installed builtin authority is invalid");
    frozen->identity_ = identity("lemonade.backend-builtin-context.v1\n", {{"ids", frozen->ids_}, {"recipes", frozen->recipes_},
        {"sections", frozen->sections_}, {"options", frozen->options_}, {"persisted", frozen->persisted_},
        {"defaults", frozen->defaults_}, {"pins", frozen->pins_}, {"exports", installed_builtin_exports().digest()}});
    return frozen;
}

std::shared_ptr<const FrozenBuiltinCatalog> freeze_builtin_catalog() { return CatalogAdmissionBuilder::freeze(); }

CatalogSnapshotResult CatalogAdmissionBuilder::snapshot(std::shared_ptr<const FrozenBuiltinCatalog> builtins,
    const std::vector<ExternalDefinitionInput>& sources, const DefinitionLimits& limits) {
    CatalogSnapshotResult result;
    const auto fail = [&](const std::string& code, const std::string& id = "", const std::string& pointer = "") { error(result.diagnostics, limits, code, id, pointer); };
    try {
        if (!builtins) { fail("invalid_snapshot"); return result; }
        auto snapshot = std::make_shared<CatalogSourceSnapshot>();
        snapshot->builtins_ = std::move(builtins);
        snapshot->limits_ = limits;
        auto& metrics = snapshot->metrics_;
        metrics.definitions = snapshot->builtins_->definitions_;
        metrics.source_bytes = snapshot->builtins_->source_bytes_;
        metrics.retained_logical_bytes = snapshot->builtins_->logical_bytes_;
        charge(metrics.definitions, sources.size(), limits.snapshot_documents);
        charge(metrics.source_bytes, 0, limits.snapshot_source_bytes);
        auto& retained = metrics.retained_logical_bytes;
        keep(retained, checked_sum(sizeof(CatalogSourceSnapshot), checked_product(sources.size(), sizeof(CatalogSourceSnapshot::Source))), limits);
        for (const auto& input : sources) charge(metrics.source_bytes, input.bytes.size(), limits.snapshot_source_bytes);
        for (const auto& input : sources) {
            const auto header = inspect_catalog_input(input.bytes, limits);
            const auto& id = header.id;
            if (snapshot->builtins_->ids_.count(id) || snapshot->ids_.count(id)) {
                fail("id_collision", id, "/id");
                if (header.version == 1 && !result.diagnostics.entries.empty()) result.diagnostics.entries.back().message = "Definition ID already owned";
                return result;
            }
            keep(retained, checked_sum(input.bytes.size(), checked_sum(sizeof(CatalogInputHeader) + 256,
                checked_sum(checked_product(id.size(), 3), checked_sum(header.recipe.size(), logical_json(header.parent))))), limits);
            CatalogSourceSnapshot::Source source{std::string(input.bytes), header, {}};
            if (header.version != 2) {
                keep(retained, v1_allowance(input.bytes.size(), header.nodes), limits);
                const auto parsed = BackendDefinitionStore().validate({"", source.bytes, DefinitionOrigin::External});
                if (!parsed.definition) {
                    for (const auto& row : parsed.diagnostics) {
                        if (result.diagnostics.entries.size() < limits.diagnostics)
                            result.diagnostics.entries.push_back({row.code, row.definition, row.pointer, row.message, {}, {}, {}});
                        else result.diagnostics.truncated = true;
                    }
                    return result;
                }
                source.v1 = parsed.definition;
            }
            snapshot->ids_.emplace(id, snapshot->sources_.size());
            snapshot->sources_.push_back(std::move(source));
        }
        std::vector<DefinitionSourceView> v2_sources;
        keep(retained, checked_product(sources.size(), sizeof(DefinitionSourceView)), limits);
        for (const auto& source : snapshot->sources_) if (source.header.version == 2) {
            const auto& parent = source.header.parent;
            if (parent.is_object() && parent.contains("external") && parent["external"].is_string()) {
                const auto name = parent["external"].get<std::string>();
                const auto found = snapshot->ids_.find(name);
                if (found == snapshot->ids_.end()) { fail("missing_parent", source.header.id, "/extends/external"); return result; }
                if (snapshot->sources_[found->second].header.version == 1) {
                    fail("unsupported_parent_version", source.header.id, "/extends/external"); return result;
                }
                keep(retained, checked_sum(name.size(), source.header.id.size() + 128), limits);
                snapshot->descendants_[name].push_back(source.header.id);
            }
            v2_sources.push_back({{DefinitionNamespace::External, source.header.id, 0}, source.bytes, ""});
        }
        const auto authority = BuiltinCatalogLoader::compose(snapshot->builtins_->authority_, v2_sources, limits, retained);
        append(result.diagnostics, authority.diagnostics, limits);
        if (!authority.snapshot) return result;
        snapshot->authority_ = authority.snapshot;
        result.snapshot = std::move(snapshot);
    } catch (const LimitExceeded&) { fail("limit_exceeded"); }
    catch (const IntakeFailure& failure) { fail(failure.code); }
    return result;
}

struct AdmittedCatalog::Impl {
    std::shared_ptr<const CatalogSourceSnapshot> sources;
    std::map<std::string, std::shared_ptr<const BackendDefinition>> v1;
    std::map<std::string, std::shared_ptr<const AuthorizedDefinition>> v2;
    CatalogAdmissionMetrics metrics;
    std::string identity;
};

CatalogAdmissionResult CatalogAdmissionBuilder::admit(std::shared_ptr<const CatalogSourceSnapshot> snapshot) {
    CatalogAdmissionResult result;
    const auto limits = snapshot ? snapshot->limits_ : DefinitionLimits{};
    const auto fail = [&](const std::string& code, const std::string& id = "", const std::string& pointer = "") { error(result.diagnostics, limits, code, id, pointer); };
    try {
        if (!snapshot) { fail("invalid_snapshot"); return result; }
        auto impl = std::make_shared<AdmittedCatalog::Impl>();
        impl->sources = snapshot;
        impl->metrics = snapshot->metrics_;
        auto& retained = impl->metrics.retained_logical_bytes;
        const auto& builtin = *snapshot->builtins_;
        keep(retained, builtin.logical_bytes_, limits);
        auto recipes = builtin.recipes_, sections = builtin.sections_, options = builtin.options_;
        auto persisted = builtin.persisted_;
        std::set<std::string> aliases;
        std::size_t rows = recipes.size();
        charge(rows, snapshot->sources_.size(), 1024);
        Json identities = Json::array();
        DefinitionAuthorizer authorizer(snapshot->authority_);
        // All v1 aliases are reserved before v2 ownership is considered, independent of file order.
        for (const auto& source : snapshot->sources_) if (source.v1) for (const auto* key : {"install_recipe", "pin_recipe"}) {
            const auto& install = source.v1->records["install"];
            if (install.contains(key)) reserve(aliases, install[key].get<std::string>(), retained, limits);
        }
        for (const auto& source : snapshot->sources_) {
            const auto& id = source.header.id;
            const Json* declaration = nullptr;
            if (source.v1) {
                keep(retained, sizeof(std::shared_ptr<const BackendDefinition>) + id.size() + 96, limits);
                impl->v1.emplace(id, source.v1);
                declaration = &source.v1->records;
                const auto& install = declaration->at("install");
                if (install["strategy"] == "github_release") {
                    const auto pin = install.value("pin_recipe", source.header.recipe);
                    const auto shipped = builtin.pins_.is_object() ? builtin.pins_.value(pin, Json::object()) : Json::object();
                    const auto versions = install.value("default_versions", Json::object());
                    std::set<std::string> backends;
                    const auto& selection = declaration->at("launch").at("backend");
                    if (selection.contains("fixed") && selection["fixed"].is_string()) backends.insert(selection["fixed"]);
                    else if (selection.contains("fixed")) for (const auto& [os, value] : selection["fixed"].items()) backends.insert(value);
                    for (const auto& row : install["cases"]) for (const auto& backend : row.value("backend", Json::array())) backends.insert(backend);
                    const auto channels = declaration->at("metadata").value("rocm_channels", std::vector<std::string>{});
                    std::set<std::string> normalized;
                    for (const auto& backend : backends) {
                        if (backend == "rocm" && !channels.empty()) for (const auto& channel : channels) normalized.insert("rocm-" + channel);
                        else if (backend != "system") normalized.insert(backend);
                    }
                    for (const auto& backend : normalized) if (!shipped.contains(backend) && (!versions.contains(backend) || versions[backend].get<std::string>().empty())) {
                        fail("required", id, "/install/default_versions");
                        if (!result.diagnostics.entries.empty()) result.diagnostics.entries.back().message = "Managed backend requires a default version";
                        return result;
                    }
                }
                identities.push_back({{"id", id}, {"version", 1}, {"definition", source.v1->digest}});
            } else {
                const auto authorized = authorizer.authorize_accounted({DefinitionNamespace::External, id, 0}, &retained);
                result.resolved = authorized.resolved;
                result.authorized = authorized.definition != nullptr;
                if (!authorized.definition) { append(result.diagnostics, authorized.diagnostics, limits); return result; }
                ++impl->metrics.authorized_definitions;
                keep(retained, id.size() + sizeof(std::shared_ptr<const AuthorizedDefinition>) + 1024, limits);
                impl->v2.emplace(id, authorized.definition);
                const auto& resolved = authorized.definition->definition();
                declaration = &resolved.declaration;
                const auto& metadata = declaration->at("metadata");
                const auto& public_options = metadata.value("options", Json::array());
                if (public_options.size() > 1024 - 7 - (metadata.value("uses_ctx_size", false) ? 1 : 0)) {
                    fail("invalid_public_projection", id, "/metadata/options"); return result;
                }
                const auto scalar = [](const Json& value) {
                    return !value.is_structured() && (!value.is_number() || (std::isfinite(value.get<double>()) &&
                        (!value.is_number_integer() || std::abs(value.get<double>()) <= 9007199254740991.0)));
                };
                for (std::size_t i = 0; i < public_options.size(); ++i) {
                    const auto& option = public_options[i];
                    bool valid = scalar(option["default_value"]);
                    for (const auto* field : {"enum", "unset_values"}) if (option.contains(field)) {
                        valid = valid && option[field].size() <= 1024;
                        for (const auto& value : option[field]) valid = valid && scalar(value);
                    }
                    valid = valid && validate_option_contract(option, option["default_value"]).empty();
                    if (option.contains("enum")) for (const auto& value : option["enum"])
                        valid = valid && validate_option_contract(option, value).empty();
                    if (option.contains("ui")) valid = valid && scalar(option["ui"]["order"]);
                    if (!valid) { fail("invalid_public_projection", id, "/metadata/options/" + std::to_string(i)); return result; }
                }
                const auto& install = declaration->at("install");
                for (const auto* key : {"install_recipe", "pin_recipe"}) if (install.contains(key) && install[key] != source.header.recipe) {
                    fail("install_ownership", id, std::string("/install/") + key); return result;
                }
                if (aliases.count(source.header.recipe) || aliases.count(id)) { fail("alias_collision", id, "/recipe"); return result; }
                identities.push_back({{"id", id}, {"version", 2}, {"source", resolved.source_digest},
                    {"definition", resolved.definition_hash}, {"dependencies", resolved.dependency_digest},
                    {"authorization", authority_identity(*authorized.definition)}});
            }
            const auto& doc = *declaration;
            const auto recipe = doc.at("recipe").get<std::string>();
            const auto section_value = doc.at("metadata").value("config_section", "");
            const auto section = section_value.empty() ? recipe : section_value;
            const auto own = [&](std::set<std::string>& values, const std::string& value, const std::string& code, const std::string& pointer) {
                if (values.count(value)) {
                    fail(code, id, pointer);
                    if (source.v1 && !result.diagnostics.entries.empty()) {
                        static const std::map<std::string, std::string> messages{{"recipe_collision", "Recipe already owned"},
                            {"section_collision", "Configuration section already owned"}, {"option_collision", "Option already owned"}};
                        const auto found = messages.find(code);
                        if (found != messages.end()) result.diagnostics.entries.back().message = found->second;
                    }
                    return false;
                }
                reserve(values, value, retained, limits);
                return true;
            };
            if (!own(recipes, recipe, "recipe_collision", "/recipe") || !own(sections, section, "section_collision", "/metadata/config_section")) return result;
            for (std::size_t i = 0; i < doc.at("metadata").value("options", Json::array()).size(); ++i) {
                const auto& option = doc["metadata"]["options"][i];
                const auto name = option["name"].get<std::string>();
                const auto pointer = "/metadata/options/" + std::to_string(i) + "/name";
                if (!own(options, name, "option_collision", pointer) ||
                    !own(persisted[section], backend_option_config_key(name), "config_key_collision", pointer)) return result;
            }
            keep(retained, logical_json(identities.back()), limits);
        }
        keep(retained, checked_sum(logical_json(identities), 1024), limits);
        impl->identity = identity("lemonade.backend-admitted-catalog.v1\n", {{"builtins", builtin.identity_},
            {"manifest", installed_definition_admission_features()}, {"definitions", identities}, {"recipes", recipes},
            {"sections", sections}, {"options", options}, {"persisted", persisted}, {"aliases", aliases}});
        auto admitted = std::shared_ptr<AdmittedCatalog>(new AdmittedCatalog());
        admitted->impl_ = std::move(impl);
        result.catalog = std::move(admitted);
    } catch (const LimitExceeded&) { fail("limit_exceeded"); }
    catch (const std::exception&) { fail("invalid_catalog"); }
    return result;
}

CandidateAdmissionResult CatalogAdmissionBuilder::candidate(std::shared_ptr<const AdmittedCatalog> active, std::string_view bytes) {
    CandidateAdmissionResult result;
    if (!active) { error(result.admission.diagnostics, DefinitionLimits{}, "invalid_snapshot"); return result; }
    const auto& original = *active->impl_->sources;
    try {
        const auto header = inspect_catalog_input(bytes, original.limits_);
        result.input_schema_version = header.version;
        result.definition_id = header.id;
        result.recipe = header.recipe;
        std::vector<ExternalDefinitionInput> inputs;
        for (const auto& source : original.sources_) {
            if (source.header.id == header.id && source.header.recipe == header.recipe) continue;
            inputs.push_back({source.bytes, ""});
        }
        inputs.push_back({bytes, ""});
        const auto replacement = snapshot(original.builtins_, inputs, original.limits_);
        result.admission = replacement.snapshot ? admit(replacement.snapshot) : CatalogAdmissionResult{nullptr, replacement.diagnostics};
    } catch (const LimitExceeded&) { error(result.admission.diagnostics, original.limits_, "limit_exceeded"); }
    catch (const IntakeFailure& failure) { error(result.admission.diagnostics, original.limits_, failure.code); }
    return result;
}

CandidateAdmissionResult validate_candidate_definition(std::shared_ptr<const AdmittedCatalog> active, std::string_view bytes) {
    return CatalogAdmissionBuilder::candidate(std::move(active), bytes);
}

CatalogSnapshotResult make_catalog_source_snapshot(std::shared_ptr<const FrozenBuiltinCatalog> builtins,
    const std::vector<ExternalDefinitionInput>& sources, const DefinitionLimits& limits) {
    return CatalogAdmissionBuilder::snapshot(std::move(builtins), sources, limits);
}
CatalogAdmissionResult admit_catalog(std::shared_ptr<const CatalogSourceSnapshot> snapshot) { return CatalogAdmissionBuilder::admit(std::move(snapshot)); }
const std::string& AdmittedCatalog::admission_id() const noexcept { return impl_->identity; }
const CatalogAdmissionMetrics& AdmittedCatalog::metrics() const noexcept { return impl_->metrics; }
const CatalogSourceSnapshot& AdmittedCatalog::sources() const noexcept { return *impl_->sources; }
std::shared_ptr<const BackendDefinition> AdmittedCatalog::v1_definition(const std::string& id) const {
    const auto found = impl_->v1.find(id);
    return found == impl_->v1.end() ? nullptr : found->second;
}
std::shared_ptr<const AuthorizedDefinition> AdmittedCatalog::v2_definition(const std::string& id) const {
    const auto found = impl_->v2.find(id);
    return found == impl_->v2.end() ? nullptr : found->second;
}

}
