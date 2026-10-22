#pragma once

#include "lemon/backends/configured/backend_catalog.h"
#include "lemon/utils/recipe_arg_resolver.h"
#include "llamacpp_v2_load_artifact_parity.h"

namespace runtime_fixture::llama_corpus {
inline const std::array<std::string, 4> views{"linux_x64", "linux_arm64", "windows_x64", "macos_arm64"};
inline const std::array<std::string, 10> version_cases{"version-408", "version-409", "version-410", "version-411",
    "version-412", "version-413", "version-414", "version-open-failure", "version-line-limit", "version-nonzero-status"};
inline const std::string probe_event = "bounded-probe:llama-server argv=[--version] stdout max_bytes=65536 timeout_ms=5000";

inline Json system_case(const Json& input, const std::string& view, const DefinitionRunner& runner) {
    const auto facts = input.value("facts", Json::object());
    Json events = Json::array(), result = Json::object();
    const auto contains = [&](const std::string& key, const std::string& path) {
        events.push_back(key + ":" + path);
        const auto values = facts.value(key, Json::array());
        return std::find(values.begin(), values.end(), path) != values.end();
    };
    auto sources = rt::probe_sources({
        [&](const std::string& key) -> std::optional<std::string> {
            events.push_back("env:" + key);
            const auto environment = facts.value("env", Json::object());
            return environment.contains(key) ? std::optional<std::string>(environment.at(key).get<std::string>()) : std::nullopt;
        },
        [&](const auto& path) { return contains("regular", path); },
        [&](const auto& path) { return contains("executable", path); },
        [&](const auto& path) { return contains("exists", path); }
    });
    sources.version = [&](std::size_t bytes, unsigned wait, const auto& cancel) {
        check(bytes == 65536 && wait == 5000 && !cancel(), "FrozenVersionProbePolicyExact:" + view);
        events.push_back(probe_event);
        return utils::CapturedProcess{facts.value("probe_failure", false) ? "" : facts.value("version_output", ""),
            facts.value("probe_status", 0), false, false, false};
    };
    const PlatformInfo platform{view.rfind("windows", 0) == 0 ? "windows" : view.rfind("macos", 0) == 0 ? "macos" : "linux",
        view.find("arm64") == std::string::npos ? "x86_64" : "aarch64", ""};
    try {
        if (input.at("kind") == "system-version") {
            result["system"] = runner.resolve_version("system", "file-version", sources, platform);
            result["managed"] = runner.resolve_version("cpu", "file-version", sources, platform);
        } else {
            const auto checked = runner.check_install("system", input.at("binary_found"), sources, platform);
            result["installed"] = checked.installed; result["message"] = checked.error;
        }
        result["error"] = "";
    } catch (const std::invalid_argument& error) { result["error"] = error.what(); result["exception"] = "invalid_argument"; }
    catch (const std::exception& error) { result["error"] = error.what(); result["exception"] = "exception"; }
    result["events"] = events; result["command"] = nullptr;
    return normalize(result);
}

inline Json scoped_case(const Json& input, const DefinitionRunner& runner) {
    utils::ScopedCustomArgs args;
    args.backend = input.value("backend_args", "--threads 2 --batch 16");
    args.architecture = input.value("arch_args", "--threads 3 --ubatch 8");
    args.model_defaults = input.value("model_defaults", "--threads 4");
    args.model = input.value("model_args", "--threads 5");
    args.request = input.value("request_args", "--threads 6");
    const auto state = input.value("request_state", "omitted");
    args.request_state = state == "value" ? utils::CustomArgsRequestState::Value : state == "tombstone" ? utils::CustomArgsRequestState::Tombstone : utils::CustomArgsRequestState::Omitted;
    args.merge_args = input.value("merge_args", true);
    RecipeOptions options("llamacpp", {{"merge_args", false}});
    options.set_option("llamacpp_backend", ""); options.set_option("llamacpp_device", "");
    options.set_option("llamacpp_args", utils::resolve_scoped_custom_args(args));
    runner.resolve_runtime_options(ModelInfo{}, options);
    return {{"args", options.get_option("llamacpp_args")}, {"error", ""}, {"events", Json::array()}, {"command", nullptr}};
}

inline Json capture(const std::string& view) {
    const auto executable = runtime_fixture::executable(Json::parse(read_bytes("llamacpp.candidate.json")));
    const auto entry = ConfiguredBackendEntry::make(executable);
    const DefinitionRunner runner(executable);
    Json results = Json::object(), counts = Json::object();
    for (const auto& input : read("cases")) {
        const auto kind = input.at("kind").get<std::string>();
        Json actual;
        if (kind == "install") actual = llamacpp_install_case(input, view, executable, *entry);
        else if (kind == "runtime") actual = llamacpp_runtime_case(input, view, executable);
        else if (kind == "load") actual = load_case(input, view, runner, entry->spec);
        else if (kind == "scoped") actual = scoped_case(input, runner);
        else if (kind == "system-version" || kind == "system-install") actual = system_case(input, view, runner);
        else if (kind == "gguf") {
            try { actual = {{"gguf", artifact_case(input, runner)}, {"error", ""}}; }
            catch (const std::invalid_argument& error) { actual = {{"error", error.what()}, {"exception", "invalid_argument"}}; }
            catch (const std::exception& error) { actual = {{"error", error.what()}, {"exception", "exception"}}; }
            actual["events"] = Json::array(); actual["command"] = nullptr;
        } else throw std::logic_error("Unknown frozen case kind: " + kind);
        const auto id = input.at("id").get<std::string>();
        if (results.contains(id)) throw std::logic_error("Duplicate frozen case");
        results[id] = std::move(actual);
        counts[kind] = counts.value(kind, 0) + 1;
    }
    check(counts == Json{{"install", 66}, {"runtime", 29}, {"load", 321}, {"gguf", 30}, {"scoped", 6},
        {"system-version", 10}, {"system-install", 14}} && results.size() == 476, "Full476CaseCensus:" + view);
    std::cout << view << " coverage " << counts.dump() << " total=" << results.size() << '\n';
    return results;
}

inline Json sidecar(const std::string& path) {
    std::ifstream input(path);
    return Json::parse(input);
}
inline void require(bool value, const std::string& message) {
    if (!value) throw std::logic_error(message);
}
inline void validate_sidecar(const Json& value) {
    require(value.size() == 3 && value.at("schema_version") == 1 && value.at("fixtures").is_object(), "SidecarEnvelopeExact");
    const auto& families = value.at("families");
    require(families.is_array() && families.size() == 1 && families.at(0).size() == 2 &&
        families.at(0).at("id") == "bounded-probe", "ExactlyOneBoundedProbeFamily");
    const auto& hashes = value.at("fixtures");
    require(hashes.size() == 6, "FrozenFixtureHashSetExact");
    for (const auto& name : std::array<std::string, 6>{"cases", "pins", "linux_x64", "linux_arm64", "windows_x64", "macos_arm64"}) {
        std::ifstream input(std::string(LEMON_TEST_FIXTURES) + "/llamacpp-oracle/" + name + ".json", std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        require(input.good() || input.eof(), "FrozenFixtureReadable");
        require(hashes.at(name + ".json") == schema_digest(bytes), "FrozenFixtureHashExact:" + name);
    }
    const auto& records = families.at(0).at("records");
    require(records.is_array() && records.size() == 40, "Exactly40ProbeRecords");
    std::set<std::pair<std::string, std::string>> used;
    for (const auto& record : records) {
        require(record.size() == 5, "ProbeRecordFieldsExact");
        const auto view = record.at("view").get<std::string>(), id = record.at("case").get<std::string>();
        require(std::find(views.begin(), views.end(), view) != views.end() &&
            std::find(version_cases.begin(), version_cases.end(), id) != version_cases.end() && record.at("pointer") == "/events",
            "OnlyTenVersionCasesAcrossFourViews");
        require(used.emplace(view, id).second, "NoDuplicateProbeRecords");
        require(record.at("old") == read(view).at("cases").at(id).at("events"), "ProbeOldValueExact");
        require(record.at("new") == Json::array({probe_event}) && record.at("old") != record.at("new"), "ProbeNewValueExactAndNecessary");
    }
}
inline Json compare(const std::string& view, const Json& actual, const Json& value) {
    auto expected = read(view).at("cases"), disclosed = expected;
    Json used = Json::array();
    for (const auto& record : value.at("families").at(0).at("records")) {
        if (record.at("view") != view) continue;
        const auto id = record.at("case").get<std::string>();
        const Json::json_pointer pointer(record.at("pointer").get<std::string>());
        require(expected.at(id).at(pointer) != actual.at(id).at(pointer), "EveryProbeRecordUsed:" + view + ":" + id);
        disclosed.at(id).at(pointer) = record.at("new");
        used.push_back(id);
    }
    const auto raw = Json::diff(expected, actual);
    require(used.size() == 10, "EveryViewUsesTenProbeRecords");
    if (raw != Json::diff(expected, disclosed)) std::cout << "Undisclosed differences " << Json::diff(disclosed, actual).dump() << '\n';
    require(raw == Json::diff(expected, disclosed), "CompleteRawDiffEqualsSidecar:" + view);
    require(disclosed == actual, "ZeroRemainingDifferences:" + view);
    return {{"view", view}, {"cases", actual.size()}, {"used", used}, {"raw_diff", raw}};
}
}
