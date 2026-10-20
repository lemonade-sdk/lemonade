#include "backend_definition_v2_descriptor.h"
#include "backend_descriptors_generated.h"
#include "lemon/backends/configured/backend_definition_resolver.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace lemon;
using namespace lemon::backends;
using Json = nlohmann::json;
const BackendDescriptor* fixture_descriptor_peer();
namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& label) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL %s\n", label.c_str()); }
}
Json dump(const BackendDescriptor& d) {
    Json options = Json::array(), support = Json::array();
    for (const auto& option : d.options) options.push_back({{"name",option.name},{"cli_flag",option.cli_flag},{"default_value",option.default_value},{"type_name",option.type_name},{"help",option.help},{"group",option.group}});
    for (const auto& row : d.support) {
        Json gates = Json::object();
        for (const auto& [arch,gate] : row.arch_gates) gates[arch] = {{"os",gate.os},{"channels",gate.channels}};
        support.push_back({{"backend",row.backend},{"tier",static_cast<int>(row.tier)},{"format",static_cast<int>(row.format)},{"supported_os",row.supported_os},{"devices",row.devices},{"device_summary",row.device_summary},{"arch_gates",gates}});
    }
    return {{"recipe",d.recipe},{"display_name",d.display_name},{"binary",d.binary},{"config_section",d.config_section},
        {"default_device",static_cast<int>(d.default_device)},{"slot_policy",static_cast<int>(d.slot_policy)},{"selectable_backend",d.selectable_backend},
        {"uses_ctx_size",d.uses_ctx_size},{"dynamic_models",d.dynamic_models},{"options",options},{"support",support},
        {"supported_modes",d.supported_modes},{"required_checkpoints",d.required_checkpoints},{"default_capabilities",d.default_capabilities},
        {"web_display_name",d.web_display_name},{"rocm_channels",d.rocm_channels},{"exposes_prometheus_metrics",d.exposes_prometheus_metrics},
        {"rocm_requires_cwsr_fix",d.rocm_requires_cwsr_fix},{"version_policy",static_cast<int>(d.version_policy)},{"self_manages_downloads",d.self_manages_downloads},
        {"takes_args",d.takes_args},{"arg_variants",d.arg_variants},{"bin_variants",d.bin_variants},{"config_extra",d.config_extra},{"streams_model_from_storage",d.streams_model_from_storage}};
}
Json read(const std::filesystem::path& path) { std::ifstream stream(path); return Json::parse(stream); }
}
namespace lemon::backends {
const ResolvedDefinition& fixture_resolved_definition() {
    static const auto resolved = [] {
        const std::string source = k_fixture_source;
        const DefinitionKey key{DefinitionNamespace::Builtin,"llamacpp",1};
        const auto snapshot = make_v2_source_snapshot({{key,source,""}});
        DefinitionResolver resolver(snapshot.snapshot);
        const auto result = resolver.resolve(key);
        if (!result.definition) throw std::logic_error("Synthetic descriptor definition did not resolve");
        return result.definition;
    }();
    return *resolved;
}
}
int main(int argc, char**) {
    Json all = Json::object();
    const auto descriptors = all_generated_descriptors();
    std::size_t definitions = 0;
    for (const auto& file : std::filesystem::directory_iterator(LEMON_PRODUCTION_DEFINITIONS)) {
        if (file.path().extension() != ".json") continue;
        const auto declaration = read(file.path());
        const auto recipe = declaration["recipe"].get<std::string>();
        const BackendDescriptor* expected = nullptr;
        for (const auto* descriptor : descriptors) if (descriptor->recipe == recipe) expected = descriptor;
        check(expected != nullptr,"production descriptor exists");
        const auto actual = descriptor_from_declaration(declaration,DefinitionNamespace::Builtin,DescriptorPlatform::native());
        check(expected && dump(actual) == dump(*expected),"every production field equals generated descriptor " + recipe);
        all[recipe] = dump(actual);
        ++definitions;
        for (const auto* os : {"windows","linux","macos"}) {
            const auto mapped = descriptor_from_declaration(declaration,DefinitionNamespace::Builtin,{os});
            const auto& binary = declaration["metadata"]["binary"];
            check(mapped.binary == (binary.is_string() ? binary.get<std::string>() : binary[os].get<std::string>()),std::string("explicit OS binary ") + os);
        }
    }
    const auto fixture_path = std::filesystem::path(LEMON_TEST_FIXTURES) / "backend-v2/llamacpp.example.json";
    std::ifstream fixture_stream(fixture_path, std::ios::binary);
    const std::string fixture_bytes{std::istreambuf_iterator<char>(fixture_stream), std::istreambuf_iterator<char>()};
    check(std::string(k_fixture_source) == fixture_bytes,"synthetic embedded bytes match fixture exactly");
    check(Json::parse(k_fixture_source) == read(fixture_path),"synthetic embedded source matches fixture");
    check(definitions == 8,"all eight definitions exercised");
    check(fixture_descriptor_peer() == &v2_fixture::descriptor,"named inline descriptor address equal across translation units");
    auto declaration = fixture_resolved_definition().declaration;
    check(dump(v2_fixture::descriptor) == dump(descriptor_from_declaration(declaration,DefinitionNamespace::Builtin,DescriptorPlatform::native())),"generated v2 initializer uses pure resolver and mapper");
    for (const auto* field : {"default_device","slot_policy","version_policy"}) {
        auto invalid = declaration; invalid["metadata"][field] = "invalid";
        bool rejected = false;
        try { descriptor_from_declaration(invalid,DefinitionNamespace::Builtin,{"linux"}); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected,std::string("unknown enum rejected ") + field);
    }
    for (const auto* field : {"tier","format"}) {
        auto invalid = declaration; invalid["metadata"]["support"][0][field] = "invalid";
        bool rejected = false;
        try { descriptor_from_declaration(invalid,DefinitionNamespace::Builtin,{"linux"}); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected,std::string("unknown support enum rejected ") + field);
    }
    declaration["metadata"]["options"] = Json::array({{{"name","fork_args"},{"default_value",Json::array({nullptr,1.25,{{"a","quoted\"\\é"}}})}},{{"name","fork_device"},{"default_value",false}}});
    const auto external = descriptor_from_declaration(declaration,DefinitionNamespace::External,{"linux"});
    check(external.config_extra["args"] == declaration["metadata"]["options"][0]["default_value"] && external.config_extra["device"] == false,"external persisted keys preserve JSON types and suffix mapping");
    all["synthetic"] = dump(external);
    if (argc > 1) std::puts(all.dump().c_str());
    std::printf("%d/%d checks passed\n",checks-failures,checks);
    return failures ? 1 : 0;
}
