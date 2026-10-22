#include "backend_definition_v2_descriptor.h"
#include "backend_descriptors_generated.h"
#include "lemon/backends/configured/backend_definition_resolver.h"
#include "llamacpp_oracle_descriptor.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

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

void check_candidate(const std::filesystem::path& path) {
    const auto envelope = read(path);
    Json ids = Json::array();
    for (const auto& option : envelope.at("merge").at("metadata").at("options")) ids.push_back(option.at("id"));
    check(ids == Json::array({"backend", "device", "args"}), "CandidateStableOptionIdsExact");
    const DefinitionKey key{DefinitionNamespace::Builtin, "llamacpp", 1};
    const auto snapshot = make_v2_source_snapshot({{key, envelope.dump(), ""}});
    DefinitionResolver resolver(snapshot.snapshot);
    const auto resolved = resolver.resolve(key);
    check(resolved.definition != nullptr, "CandidateResolvesThroughV2Schema");
    if (!resolved.definition) {
        for (const auto& diagnostic : resolved.diagnostics.entries)
            std::printf("%s %s\n", diagnostic.code.c_str(), diagnostic.pointer.c_str());
        return;
    }
    const auto& declaration = resolved.definition->declaration;
    const auto& metadata = declaration.at("metadata");
    for (const auto* view : {"linux", "windows", "macos"}) {
        const auto actual = descriptor_from_declaration(declaration, DefinitionNamespace::Builtin, {view});
        const auto expected = read(std::filesystem::path(LEMON_TEST_FIXTURES) / "llamacpp-oracle" /
            (std::string("descriptor-") + view + ".json"));
        check(dump_llamacpp_descriptor(actual).dump() == expected.dump(), std::string("CandidateAllDescriptorFieldsAndTypesExact ") + view);
    }
    check(metadata.at("server_name") == "llama-server" && metadata.at("log_category") == "LlamaCpp" &&
        metadata.at("log_level_policy") == "passthrough", "CandidateServerMetadataExact");
    const auto alias = Json{{"id", "llamacpp.max_tokens_alias"}, {"params", Json::object()}};
    const auto sanitizer = Json{{"id", "llamacpp.tool_schema_limits"}, {"params", Json::object()}};
    const auto normalization = Json{{"id", "llamacpp.client_model"}, {"params", Json::object()}};
    struct Cell { const char* id; const char* operation; const char* path; Json request; };
    const std::vector<Cell> cells = {
        {"chat.buffered", "chat_completion", "/v1/chat/completions", Json::array({alias, sanitizer})},
        {"completion.buffered", "completion", "/v1/completions", Json::array({alias})},
        {"responses.buffered", "responses", "/v1/responses", Json::array({sanitizer})},
        {"chat.streaming", "chat_completion", "/v1/chat/completions", Json::array({alias, sanitizer})},
        {"completion.streaming", "completion", "/v1/completions", Json::array()},
        {"responses.streaming", "responses", "/v1/responses", Json::array({sanitizer})},
        {"embedding.buffered", "embeddings", "/v1/embeddings", Json::array()},
        {"reranking.buffered", "reranking", "/v1/rerank", Json::array()},
        {"slots.buffered", "slots", "/slots", Json::array()},
        {"slot-action.buffered", "slot_action", "/slots/{slot_id}?action={action}", Json::array()},
        {"tokenize.buffered", "tokenize", "/tokenize", Json::array()}
    };
    const auto& operations = declaration.at("operations");
    check(operations.size() == cells.size(), "CandidateAll11CellsExact");
    for (std::size_t i = 0; i < cells.size() && i < operations.size(); ++i) {
        const auto& op = operations.at(i);
        const auto& expected = cells.at(i);
        const bool streaming = i >= 3 && i <= 5;
        const Json transport = {{"kind", streaming ? "http_sse" : "http_json"},
            {"method", i == 8 ? "GET" : "POST"}, {"path", expected.path}, {"port", "http"}, {"timeout", "global"},
            {streaming ? "stream_codec" : "response_codec", streaming ? "openai_sse.v1" : "json.v1"}};
        check(op.at("id") == expected.id && op.at("operation") == expected.operation &&
            op.at("delivery") == (streaming ? "streaming" : "buffered") && op.at("model_modes") == Json::array({"*"}) &&
            op.at("transport") == transport, std::string("CandidateCellSignatureExact ") + expected.id);
        const auto& request = op.at("request");
        check(request.at("base") == "identity" && request.value("hooks", Json::array()) == expected.request &&
            request.value("set", Json::object()).empty() && request.value("remove", Json::array()).empty(),
            std::string("CandidateRequestChainExact ") + expected.id);
        check(op.value("response", Json::object()).value("hooks", Json::array()) == (i < 3 ? Json::array({normalization}) : Json::array()),
            std::string("CandidateResponseNormalizationStageExact ") + expected.id);
    }
    const auto& launch = declaration.at("launch");
    check(launch.at("prepare_order") == Json::array({"backend", "install", "hook", "artifacts", "ports", "program"}),
        "CandidatePreparationOrderExact");
    check(launch.at("context_floor") == Json{{"embedding", 8192}}, "CandidateEmbeddingContextFloorExact");
    const auto& argv = launch.at("argv");
    ids = Json::array();
    for (const auto& arg : argv) ids.push_back(arg.at("id"));
    const auto argv_ids = Json::array({"model-hf", "model-local", "ctx", "device", "port", "jinja", "metrics", "debug",
        "vision", "vision-cpu", "draft", "embedding", "reranking"});
    check(ids == argv_ids, "CandidateArgvOrderExact");
    if (ids != argv_ids) return;
    const Json aliases = Json::array({Json::array({"--hf-repo", "-mr", "--hf-file", "-mf"}), Json::array({"--model"}),
        Json::array({"-c"}), Json::array({"-dev"}), Json::array(), Json::array({"--no-jinja"}), Json::array(), Json::array(),
        Json::array({"-mm", "-mmu", "--mmproj-url", "--no-mmproj", "--mmproj-auto", "--no-mmproj-auto"}), Json::array(),
        Json::array({"-md", "--spec-draft-model"}), Json::array({"--embedding"}), Json::array({"--rerank"})});
    for (std::size_t i = 0; i < argv.size() && i < aliases.size(); ++i)
        check(argv.at(i).value("aliases", Json::array()) == aliases.at(i), "CandidateReservedAliasesExact " + std::to_string(i));
    check(argv.at(0).at("reserve") == "when_active" && argv.at(1).at("reserve") == "when_active" &&
        argv.at(3).at("reserve") == "always" && argv.at(8).at("reserve") == "always" &&
        argv.at(10).at("reserve") == "always" && argv.at(11).at("reserve") == "always" &&
        argv.at(12).at("reserve") == "always", "CandidateInactiveAliasesRemainReserved");
    check(argv.at(7).at("args") == Json::array({"-lv", "4"}) && !argv.at(7).contains("flag"),
        "CandidateDebugArgvDoesNotReserveVerbosityFlag");
    const auto expected_argv = Json::parse(R"json([
        {"id":"model-hf","flag":"-hf","value":{"ref":"/model/checkpoint"},"when":{"ref":"/launch/hook/hf_load"},"aliases":["--hf-repo","-mr","--hf-file","-mf"],"reserve":"when_active"},
        {"id":"model-local","flag":"-m","value":{"ref":"/artifacts/main"},"when":{"not":{"ref":"/launch/hook/hf_load"}},"aliases":["--model"],"reserve":"when_active"},
        {"id":"ctx","flag":"--ctx-size","value":{"string":{"ref":"/launch/hook/context_size"}},"aliases":["-c"]},
        {"id":"device","flag":"--device","value":{"option":"device","type":"string"},"when":{"not":{"eq":[{"option":"device"},""]}},"aliases":["-dev"],"reserve":"always"},
        {"id":"port","flag":"--port","value":{"string":{"ref":"/launch/ports/http"}}},
        {"id":"jinja","flag":"--jinja","aliases":["--no-jinja"]},
        {"id":"metrics","flag":"--metrics"},
        {"id":"debug","args":["-lv","4"],"when":{"any":[{"eq":[{"ref":"/launch/log_level"},"debug"]},{"eq":[{"ref":"/launch/log_level"},"trace"]}]}},
        {"id":"vision","flag":"--mmproj","value":{"ref":"/artifacts/mmproj"},"when":{"ref":"/launch/hook/use_mmproj"},"aliases":["-mm","-mmu","--mmproj-url","--no-mmproj","--mmproj-auto","--no-mmproj-auto"],"reserve":"always"},
        {"id":"vision-cpu","flag":"--no-mmproj-offload","when":{"ref":"/launch/hook/mmproj_on_cpu"},"reserve":"when_active"},
        {"id":"draft","flag":"--model-draft","value":{"ref":"/artifacts/draft"},"when":{"ref":"/launch/hook/use_draft_checkpoint"},"aliases":["-md","--spec-draft-model"],"reserve":"always"},
        {"id":"embedding","flag":"--embeddings","when":{"ref":"/launch/hook/embedding"},"aliases":["--embedding"],"reserve":"always"},
        {"id":"reranking","flag":"--reranking","when":{"ref":"/launch/hook/reranking"},"aliases":["--rerank"],"reserve":"always"}
    ])json");
    check(argv == expected_argv, "CandidateAllArgvRecordsExact");
    const auto& custom = launch.at("custom_args");
    const Json defaults = Json::array({
        {{"id", "spec-dflash"}, {"args", "--spec-type draft-dflash"}, {"flag", "--spec-type"},
            {"when", {{"ref", "/runtime/dflash"}}}, {"stage", "runtime_options"}, {"respect_merge_args", true}},
        {{"id", "spec-mtp"}, {"args", "--spec-type draft-mtp"}, {"flag", "--spec-type"},
            {"when", {{"ref", "/runtime/mtp"}}}, {"stage", "runtime_options"}, {"respect_merge_args", true}},
        {{"id", "parallel"}, {"args", "--parallel 1"}, {"flag", "--parallel"}, {"aliases", {"-np"}},
            {"stage", "runtime_options"}, {"respect_merge_args", true}}
    });
    check(custom.at("defaults") == defaults, "CandidateRuntimeDefaultOrderConditionsAndStagesExact");
    check(custom.at("option") == "args" && custom.at("reserved").empty() &&
        custom.at("error_prefix") == "Invalid custom llama-server arguments:\n", "CandidateCustomArgumentPolicyExact");
    check(declaration.at("artifacts").at("main").at("strategy") == "resolved_path" &&
        declaration.at("artifacts").at("main").at("hook").at("id") == "llamacpp.artifacts.v1",
        "CandidateLoadUsesResolvedMainAndTypedManagementContract");
    const auto& cases = declaration.at("install").at("cases");
    check(cases.size() == 14, "CandidateAll14ReleaseRowsPresent");
    struct Release { const char* backend; const char* os; const char* machine; const char* prefix; const char* fact; const char* suffix; };
    const std::vector<Release> releases = {
        {"rocm-stable", "windows", "", "-bin-win-rocm-", "therock_version", "-x64.zip"},
        {"rocm-stable", "linux", "", "-bin-ubuntu-rocm-", "therock_version", "-x64.tar.gz"},
        {"rocm-nightly", "windows", "", "-windows-rocm-", "rocm_asset_family", "-x64.zip"},
        {"rocm-nightly", "linux", "", "-ubuntu-rocm-", "rocm_asset_family", "-x64.zip"},
        {"cuda", "windows", "", "-windows-cuda-", "cuda_architecture", "-x64.7z"},
        {"cuda", "linux", "x86_64", "-ubuntu-cuda-", "cuda_architecture", "-x64.tar.xz"},
        {"cuda", "linux", "aarch64", "-ubuntu-cuda-", "cuda_architecture", "-arm64.tar.xz"},
        {"metal", "macos", "", "-bin-macos-arm64.tar.gz", "", ""},
        {"cpu", "windows", "", "-bin-win-cpu-x64.zip", "", ""},
        {"cpu", "linux", "x86_64", "-bin-ubuntu-x64.tar.gz", "", ""},
        {"cpu", "linux", "aarch64", "-bin-ubuntu-arm64.tar.gz", "", ""},
        {"vulkan", "windows", "", "-bin-win-vulkan-x64.zip", "", ""},
        {"vulkan", "linux", "x86_64", "-bin-ubuntu-vulkan-x64.tar.gz", "", ""},
        {"vulkan", "linux", "aarch64", "-bin-ubuntu-vulkan-arm64.tar.gz", "", ""}
    };
    for (std::size_t i = 0; i < releases.size() && i < cases.size(); ++i) {
        const auto& row = cases.at(i);
        const auto& release = releases.at(i);
        const auto backend = row.at("backend").at(0).get<std::string>();
        const auto os = row.at("os").at(0).get<std::string>();
        check(row.at("backend").size() == 1 && row.at("os").size() == 1 &&
            (os != "windows" || !row.contains("machine_arch")), "CandidateWindowsAssetsDoNotRestrictMachineArchitecture");
        check(row.at("repo") == (backend == "rocm-nightly" ? "lemonade-sdk/llamacpp-rocm" :
            backend == "rocm-stable" || backend == "cuda" ? "lemonade-sdk/llama.cpp" : "ggml-org/llama.cpp"),
            "CandidateReleaseRepositoryExact " + row.at("id").get<std::string>());
        const auto machine = std::string(release.machine);
        const auto id = std::string(release.backend) + "-" + release.os + (machine.empty() ? "" : "-" + machine);
        auto concat = Json::array({"llama-", {{"ref", "/install/version"}}, release.prefix});
        if (std::string(release.fact) != "") {
            concat.push_back({{"ref", std::string("/install/") + release.fact}});
            concat.push_back(release.suffix);
        }
        Json expected = {{"id", id}, {"backend", {release.backend}}, {"os", {release.os}},
            {"repo", row.at("repo")}, {"filename", {{"concat", concat}}}};
        if (!machine.empty()) expected["machine_arch"] = {machine};
        check(row == expected, "CandidateReleaseSelectorsAndFilenameExact " + id);
    }
}
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
int main(int argc, char** argv) {
    const auto candidate = argc == 3 && std::string(argv[1]) == "--candidate" ? std::filesystem::path(argv[2]) :
        std::filesystem::path(LEMON_TEST_FIXTURES) / "backend-v2/llamacpp.candidate.json";
    check_candidate(candidate);
    Json all = Json::object();
    const auto descriptors = all_generated_descriptors();
    std::size_t definitions = 0;
    for (const auto& file : std::filesystem::directory_iterator(LEMON_PRODUCTION_DEFINITIONS)) {
        if (file.path().extension() != ".json") continue;
        const auto source = read(file.path());
        const auto declaration = source.at("schema_version") == 2 ?
            builtin_resolved_source({DefinitionNamespace::Builtin, "llamacpp", 1}).declaration : source;
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
    check(definitions == 9,"all nine definitions exercised");
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
    if (argc > 1 && std::string(argv[1]) != "--candidate") std::puts(all.dump().c_str());
    std::printf("%d/%d checks passed\n",checks-failures,checks);
    return failures ? 1 : 0;
}
