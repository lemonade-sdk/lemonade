// Proves the loom recipe's bundle planning, launch, request shaping, readiness,
// install and catalog contracts without a GPU, a network or a running loom-serve.

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/loom/loom.h"
#include "lemon/backends/loom/loom_bundle.h"
#include "lemon/backends/loom/loom_server.h"
#include "lemon/models/json_catalog.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace loom = lemon::backends::loom;
using json = nlohmann::json;
using lemon::backends::LoomServer;

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

bool throws_exception(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

const json kCard = json::parse(R"({
  "schema_version": 1,
  "model": {"id": "Qwen/Qwen3.6-27B"},
  "programs": {"source": {"inputs": [{"path": "program/prepare.loom", "data": ["weights.loom"]}]}},
  "services": {"chat": {"kind": "text"}},
  "variants": {"gguf": {"services": {"chat": {
    "parameters": {
      "model": {"files": [{"path": "parameter/model.gguf", "format": "gguf",
                           "origin": "@checkpoint/Qwen3.6-27B-Q4_K_M.gguf"}]},
      "draft": {"files": [{"path": "parameter/draft.gguf", "format": "gguf",
                           "origin": "@drafter/Qwen3.6-27B-DFlash-Q4_K_M.gguf"}]}
    },
    "tokenizers": {"text": "tokenizer.json"}
  }}}},
  "presets": {"chat": {"execution": {"context_capacity": 262144, "max_tokens": 4096}}},
  "origins": {
    "checkpoint": {"kind": "hf", "repository": "ggml-org/Qwen3.6-27B-GGUF",
                   "revision": "1132d03976d88e30a09ab0ddbf1531d5e1b46e7c"},
    "drafter": {"kind": "hf", "repository": "Anbeeld/Qwen3.6-27B-DFlash-GGUF",
                "revision": "f14c6b308ee71deb2b2cb47c331e164271968e6b"}
  }
})");

const json kLock = json::parse(R"({
  "schema_version": 2,
  "origins": {
    "checkpoint": {"resolved": {"revision": "1132d03976d88e30a09ab0ddbf1531d5e1b46e7c"}},
    "drafter": {"resolved": {"revision": "f14c6b308ee71deb2b2cb47c331e164271968e6b"}}
  },
  "files": [
    {"origin": "@checkpoint/Qwen3.6-27B-Q4_K_M.gguf", "size": 19095766304,
     "sha256": "65b753ea835627f7b511143c6ceb976525c7f21f5df8c664bc0a9c23d1c49921"},
    {"origin": "@drafter/Qwen3.6-27B-DFlash-Q4_K_M.gguf", "size": 1033066592,
     "sha256": "9600166778e487031cc711b30c0ec098df91156ec9bfda177185637784935c18"},
    {"origin": "tokenizer.json", "size": 9832586,
     "sha256": "cf360f770fae77c3e5e783bce81eb01aebda1627cdc38736b4a0d30f8c5f4fe7"}
  ]
})");

void check_bundle_plan() {
    const loom::BundlePlan plan = loom::plan_bundle(kCard, kLock);
    check("Plan carries the card's model id", plan.model_id == "Qwen/Qwen3.6-27B");
    check("A card with a chat preset runs it", plan.preset == "chat");
    check("Plan lists one file per pinned parameter", plan.origin_files.size() == 2);
    if (plan.origin_files.size() == 2) {
        const auto& model = plan.origin_files[0].path == "parameter/model.gguf"
                                ? plan.origin_files[0]
                                : plan.origin_files[1];
        check("A parameter keeps its run-folder path, repository, revision and file",
              model.path == "parameter/model.gguf" &&
                  model.repository == "ggml-org/Qwen3.6-27B-GGUF" &&
                  model.revision == "1132d03976d88e30a09ab0ddbf1531d5e1b46e7c" &&
                  model.file == "Qwen3.6-27B-Q4_K_M.gguf");
        check("A parameter carries its locked size and digest",
              model.size == 19095766304ull &&
                  model.sha256 ==
                      "65b753ea835627f7b511143c6ceb976525c7f21f5df8c664bc0a9c23d1c49921");
    }
    check("Bundle-local locked files are verified too",
          plan.locked_files.size() == 1 && plan.locked_files[0].path == "tokenizer.json");

    json single_preset = kCard;
    single_preset["presets"] = {{"vision-mtp", {{"execution", json::object()}}}};
    check("A card whose only preset is not chat names that preset",
          loom::plan_bundle(single_preset, kLock).preset == "vision-mtp");

    json several_presets = kCard;
    several_presets["presets"]["chat-r9700"] = {{"execution", json::object()}};
    check("A card with several presets still runs chat",
          loom::plan_bundle(several_presets, kLock).preset == "chat");

    json gpu_variant_only = kCard;
    gpu_variant_only["presets"] = {{"vision", json::object()}, {"vision-r9700", json::object()}};
    check("A card without chat runs its one preset that is not a GPU variant",
          loom::plan_bundle(gpu_variant_only, kLock).preset == "vision");

    json no_chat = kCard;
    no_chat["presets"] = {{"a", json::object()}, {"b", json::object()}};
    check("A card with several presets and no chat leaves the choice to loom-serve",
          loom::plan_bundle(no_chat, kLock).preset.empty());

    json url_origin = kCard;
    url_origin["origins"]["drafter"]["kind"] = "url";
    check("Non-Hugging-Face origins are rejected",
          throws_exception([&] { (void)loom::plan_bundle(url_origin, kLock); }));

    json missing_digest = kLock;
    missing_digest["files"].erase(1);
    check("A parameter without a locked digest is rejected",
          throws_exception([&] { (void)loom::plan_bundle(kCard, missing_digest); }));

    json escaping = kCard;
    escaping["variants"]["gguf"]["services"]["chat"]["parameters"]["model"]["files"][0]["path"] =
        "../outside.gguf";
    check("A parameter placed outside the run folder is rejected",
          throws_exception([&] { (void)loom::plan_bundle(escaping, kLock); }));

    json undeclared = kCard;
    undeclared["origins"].erase("drafter");
    check("A parameter naming an undeclared origin is rejected",
          throws_exception([&] { (void)loom::plan_bundle(undeclared, kLock); }));

    check("A card without a model id is rejected",
          throws_exception([&] { (void)loom::plan_bundle(json::object(), kLock); }));
}

void check_paths() {
    check("Nested relative paths are contained", loom::is_contained_relative_path("parameter/x.gguf"));
    for (const std::string path : {"", "/abs", "../x", "a/../b", "a//b", "C:/x", "a\\b", "./x"}) {
        check("Path is rejected: '" + path + "'", !loom::is_contained_relative_path(path));
    }
    check("Store folder joins org and name",
          loom::store_dir_name("loom-fun/unsloth-Qwen3.5-0.8B-loom") ==
              "loom-fun--unsloth-Qwen3.5-0.8B-loom");
    for (const std::string repo : {"noslash", "a/b/c", "/name", "org/", "org/..", "org/a b"}) {
        check("Repository id is rejected: '" + repo + "'",
              throws_exception([&] { (void)loom::store_dir_name(repo); }));
    }
}

void check_launch_contract() {
    const std::vector<std::string> expected = {
        "--model=/run/model.json",
        "--listen=127.0.0.1:14123",
        "--device=amdgpu://",
        "--preset=vision-mtp",
        "--memory_bytes=57000000000",
        "--workers=4",
    };
    check("Launch argv binds loopback and appends benign custom flags",
          loom::build_server_argv("/run", 14123, "vision-mtp", "amdgpu://",
                                  {"--memory_bytes=57000000000"}, "--workers=4") == expected);
    check("Launch argv omits an unset preset and serve flags",
          loom::build_server_argv("/run", 1, "", "amdgpu://", {}, "") ==
              std::vector<std::string>({"--model=/run/model.json", "--listen=127.0.0.1:1",
                                        "--device=amdgpu://"}));
    check("Serve flags are passed through in order",
          loom::build_server_argv("/run", 1, "", "amdgpu://",
                                  {"--pool_backing=fixed", loom::kPreloadFlag}, "") ==
              std::vector<std::string>({"--model=/run/model.json", "--listen=127.0.0.1:1",
                                        "--device=amdgpu://", "--pool_backing=fixed",
                                        "--parameter_mode=preload"}));

    for (const std::string custom : {"--listen=0.0.0.0:80", "--model=other.json", "--device=task",
                                     "--devices=d.json", "--memory_bytes=1", "--preset=x",
                                     "--flagfile=f", "--listen 0.0.0.0:80"}) {
        check("Reserved custom argument is rejected: " + custom, throws_exception([&] {
                  (void)loom::build_server_argv("/run", 1, "", "amdgpu://", {}, custom);
              }));
    }
    check("A device with whitespace is rejected", throws_exception([] {
              (void)loom::build_server_argv("/run", 1, "", "amdgpu:// --listen=0.0.0.0:1", {}, "");
          }));
    check("An empty device is rejected",
          throws_exception([] { (void)loom::build_server_argv("/run", 1, "", "", {}, ""); }));
}

void check_device_targets() {
    check("Strix Halo runs the strix-halo build",
          loom::device_target("gfx1151", "110501") == loom::kStrixHaloTarget);
    check("A gfx1201 ISA runs the radeon-r9700 build",
          loom::device_target("gfx1201", "120001") == loom::kRadeonR9700Target);
    check("A Windows R9700 named by family runs the radeon-r9700 build",
          loom::device_target("gfx120X", "AMD Radeon AI PRO R9700") == loom::kRadeonR9700Target);
    check("A gfx1201 ISA with 32 GB runs the radeon-r9700 build",
          loom::device_target("gfx1201", "120001", 31.9) == loom::kRadeonR9700Target);
    check("A gfx1201 ISA with 16 GB runs the radeon-rx9070xt build",
          loom::device_target("gfx1201", "120001", 15.9) == loom::kRadeonRx9070XtTarget);
    check("A Windows RX 9070 XT named by family runs the radeon-rx9070xt build",
          loom::device_target("gfx120X", "AMD Radeon RX 9070 XT") == loom::kRadeonRx9070XtTarget);
    check("A gfx1200 card has no build", loom::device_target("gfx1200", "120000").empty());
    check("A Windows RX 9060 named by family has no build",
          loom::device_target("gfx120X", "AMD Radeon RX 9060 XT").empty());
    check("Other GPUs have no build", loom::device_target("gfx1100", "110000").empty());

    const json info = json::parse(R"({"devices":{
        "strix-halo":{"gates":{}},
        "radeon-r9700":{"required_bytes":82400000000,"state":"unsupported"}}})");
    check("A gated target is supported", loom::bundle_supports_target(info, loom::kStrixHaloTarget));
    check("A target marked unsupported is refused",
          !loom::bundle_supports_target(info, loom::kRadeonR9700Target));
    check("A GPU the card marks as not a target is refused",
          !loom::bundle_supports_target(json::parse(R"({"devices":{"strix-halo":{"state":"not_a_target"}}})"),
                                        loom::kStrixHaloTarget));
    check("A target the bundle does not list is refused",
          !loom::bundle_supports_target(json::parse(R"({"devices":{"strix-halo":{}}})"),
                                        loom::kRadeonR9700Target));
    check("A bundle without a devices map is left to loom-serve",
          loom::bundle_supports_target(json::object(), loom::kRadeonR9700Target) &&
              loom::bundle_supports_target(json(), loom::kRadeonR9700Target));
}

void check_request_contract() {
    const json disabled = {
        {"model", "Qwen3.6-27B-Loom"},
        {"reasoning_effort", "none"},
        {"chat_template_kwargs", {{"enable_thinking", false}}},
        {"messages", {{{"role", "user"}, {"content", "/no_think\nHello"}}}},
    };
    const json prepared = loom::prepare_chat_request(disabled, "Qwen/Qwen3.6-27B");
    check("Requests address the card's model id", prepared["model"] == "Qwen/Qwen3.6-27B");
    check("Disabled thinking reaches loom-serve as the template switch only",
          prepared["chat_template_kwargs"]["enable_thinking"] == false &&
              !prepared.contains("reasoning_effort") &&
              prepared["messages"][0]["content"] == "Hello");

    const json enabled = {
        {"model", "x"},
        {"reasoning_effort", "none"},
        {"messages", {{{"role", "user"}, {"content", "/no_think\nHello"}}}},
    };
    const json untouched = loom::prepare_chat_request(enabled, "Qwen/Qwen3.6-27B");
    check("Without the template switch the request keeps the user's text and effort",
          untouched["reasoning_effort"] == "none" &&
              untouched["messages"][0]["content"] == "/no_think\nHello");
}

void check_readiness_contract() {
    const json empty_models = json::parse(R"({"object":"list","data":[]})");
    const json preparing = json::parse(
        R"({"object":"list","data":[{"id":"Qwen/Qwen3.5-0.8B","loom":{"registration":"preparing"}}]})");
    const json failed = json::parse(R"({"object":"list","data":[{"id":"Qwen/Qwen3.5-0.8B",
        "loom":{"registration":"failed","error":{"code":3,"message":"LOWERING/036: rejected"}}}]})");
    const json ready_models =
        json::parse(R"({"object":"list","data":[{"id":"Qwen/Qwen3.5-0.8B"}]})");

    check("A preparing bundle is pending",
          loom::parse_readiness(empty_models, preparing, "Qwen/Qwen3.5-0.8B").state ==
              loom::Readiness::Pending);
    const auto failure = loom::parse_readiness(empty_models, failed, "Qwen/Qwen3.5-0.8B");
    check("A failed registration reports loom-serve's reason",
          failure.state == loom::Readiness::Failed && failure.error == "LOWERING/036: rejected");
    check("A listed model is ready",
          loom::parse_readiness(ready_models, preparing, "Qwen/Qwen3.5-0.8B").state ==
              loom::Readiness::Ready);
    check("Another model's failure does not fail this load",
          loom::parse_readiness(empty_models, failed, "Qwen/Other").state ==
              loom::Readiness::Pending);
    check("Unreachable endpoints are pending",
          loom::parse_readiness(json(), json(), "Qwen/Qwen3.5-0.8B").state ==
              loom::Readiness::Pending);
}

void check_startup_failure_detail() {
    const std::string device_error =
        "runtime/src/iree/hal/drivers/amd/driver.c:146: UNIMPLEMENTED; "
        "[gpu.endpoint_query_info] libamdf API status 0x00000003";
    check("A device error is reported verbatim",
          loom::startup_failure_detail({device_error + "\r", ""}) == device_error);
    check("Only the last four printed lines are kept",
          loom::startup_failure_detail({"a", "  ", "b", "c", "d", "e"}) == "b; c; d; e");
    check("Silent exits give no detail", loom::startup_failure_detail({"", " \r"}).empty());
    check("Long output is cut from the front",
          loom::startup_failure_detail({std::string(1500, 'x')}).size() == 1003);
}

void check_card_metadata() {
    const json info = json::parse(R"({"card":{"memory_bytes":57000000000,"features":{
        "image_input":{"status":"yes"},"tool_calling":{"status":"yes"},
        "thinking":{"status":"yes"},"audio":{"status":"n/a"},
        "speculative_decoding":{"status":"yes","value":{"method":"dflash"}}}}})");
    using Args = std::vector<std::string>;
    check("Strix Halo serves a fixed pool and preloads a package that fits beside it",
          loom::serve_args(loom::kStrixHaloTarget, info, 23000000000ull, "", true) ==
              Args({"--pool_backing=fixed", "--parameter_mode=preload"}));
    check("Strix Halo keeps the default parameter mode above 40 GB of weights",
          loom::serve_args(loom::kStrixHaloTarget, info, 80400000000ull, "", true) ==
              Args({"--pool_backing=fixed"}));
    check("The R9700 serves a fixed pool and preloads any package",
          loom::serve_args(loom::kRadeonR9700Target, info, 80400000000ull, "", true) ==
              Args({"--pool_backing=fixed", "--parameter_mode=preload"}));
    check("The RX 9070 XT serves its 15 GB budget and small slabs, and preloads",
          loom::serve_args(loom::kRadeonRx9070XtTarget, info, 1, "", true) ==
              Args({"--memory_bytes=15000000000", "--state_slab_bytes=4mib",
                    "--weight_slab_bytes=64mib", "--parameter_mode=preload"}));
    check("No preload where the host has no memlock problem",
          loom::serve_args(loom::kRadeonR9700Target, info, 1, "", false) ==
              Args({"--pool_backing=fixed"}));
    check("A requested size serves an elastic pool of that size",
          loom::serve_args(loom::kRadeonR9700Target, info, 1, "16gib", true) ==
              Args({"--parameter_mode=preload", "--memory_bytes=16gib"}));
    const json recorded = json::parse(
        R"({"devices":{"strix-halo":{"serve_args":["--memory_bytes=15000000000","--state_slab_bytes=4mib"]}}})");
    check("Flags the card records for a GPU are served as they are",
          loom::serve_args(loom::kStrixHaloTarget, recorded, 1, "", true) ==
              Args({"--memory_bytes=15000000000", "--state_slab_bytes=4mib"}));
    check("A size the user asks for replaces the recorded one",
          loom::serve_args(loom::kStrixHaloTarget, recorded, 1, "8gib", true) ==
              Args({"--state_slab_bytes=4mib", "--memory_bytes=8gib"}));

    json r9700_card = kCard;
    r9700_card["presets"]["chat-r9700"] = {{"execution", json::object()}};
    check("The R9700 runs the card's -r9700 preset",
          loom::target_preset(r9700_card, "chat", loom::kRadeonR9700Target) == "chat-r9700");
    r9700_card["presets"]["chat-9070xt"] = {{"execution", json::object()}};
    check("The RX 9070 XT runs the card's -9070xt preset",
          loom::target_preset(r9700_card, "chat", loom::kRadeonRx9070XtTarget) == "chat-9070xt");
    check("Strix Halo runs the base preset",
          loom::target_preset(r9700_card, "chat", loom::kStrixHaloTarget) == "chat");
    check("The R9700 runs the base preset when the card has no variant",
          loom::target_preset(kCard, "chat", loom::kRadeonR9700Target) == "chat");
    check("Parameter bytes sum the origin files",
          loom::parameter_bytes(loom::plan_bundle(kCard, kLock)) == 19095766304ull + 1033066592ull);
    check("Verified features become catalog labels",
          loom::suggested_labels(info, "Qwen3.6-27B-Loom") ==
              std::vector<std::string>({"chat", "vision", "tool-calling", "reasoning", "dflash"}));
    check("Search names drop the GGUF publisher and the -loom suffix",
          loom::suggested_name("loom-fun/ggml-org-Qwen3.6-27B-loom", kCard) == "Qwen3.6-27B-Loom");
    check("Bundle size sums the locked files",
          loom::locked_bytes(kLock) == 19095766304ull + 1033066592ull + 9832586ull);
}

void check_installer_contract() {
#if defined(_WIN32)
    const auto params = LoomServer::get_install_params("rocm", "v0.2.0");
    check("Windows installs the win_amd64 wheel",
          params.filename == "hrx_loom-0.2.0-py3-none-win_amd64.whl");
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    const auto params = LoomServer::get_install_params("rocm", "v0.2.0");
    check("Linux installs the manylinux wheel",
          params.filename == "hrx_loom-0.2.0-py3-none-manylinux_2_39_x86_64.whl");
#endif
#if defined(_WIN32) || (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)))
    check("The wheel comes from the hrx-system release", params.repo == "ROCm/hrx-system");
    check("Only the wheel's scripts directory is installed",
          params.archive_subdir == "hrx_loom-0.2.0.data/scripts");
#endif
    check("Another backend is rejected",
          throws_exception([] { (void)LoomServer::get_install_params("vulkan", "v0.2.0"); }));

    const fs::path staging = fs::temp_directory_path() / "lemonade-loom-contract.staging";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "hrx_loom-0.2.0.data" / "scripts");
    fs::create_directories(staging / "loom");
    std::ofstream(staging / "hrx_loom-0.2.0.data" / "scripts" / "loom-serve") << "binary";
    std::ofstream(staging / "loom" / "dsl.py") << "python";
    lemon::backends::BackendUtils::keep_archive_subdir(staging.string(),
                                                       "hrx_loom-0.2.0.data/scripts");
    check("The kept directory becomes the install root",
          fs::exists(staging / "loom-serve") && !fs::exists(staging / "loom") &&
              !fs::exists(staging / "hrx_loom-0.2.0.data"));
    check("An escaping subdirectory is rejected", throws_exception([&] {
              lemon::backends::BackendUtils::keep_archive_subdir(staging.string(), "../x");
          }));
    check("A missing subdirectory is rejected", throws_exception([&] {
              lemon::backends::BackendUtils::keep_archive_subdir(staging.string(), "absent");
          }));
    fs::remove_all(staging, ec);
}

void check_descriptor_contract() {
    const auto& support = loom::descriptor.support;
    check("Loom has one experimental native variant",
          support.size() == 1 && support[0].backend == "rocm" &&
              support[0].tier == lemon::BackendTier::Experimental);
    if (support.size() == 1) {
        const auto gpus = support[0].devices.find("amd_gpu");
        check("Loom supports Strix Halo and the R9700's family",
              gpus != support[0].devices.end() && gpus->second.size() == 2 &&
                  gpus->second.count("gfx1151") == 1 && gpus->second.count("gfx120X") == 1);
    }
    check("Loom pulls its own bundles", loom::descriptor.self_manages_downloads);

    std::ifstream versions_stream(LOOM_TEST_BACKEND_VERSIONS_PATH);
    const auto versions = json::parse(versions_stream);
    check("Bundles are fetched for the pinned Loom release",
          versions["loom"]["rocm"] == loom::kBundleRevision);
}

void check_catalog_contract() {
    std::ifstream stream(LOOM_TEST_CATALOG_PATH);
    const auto catalog = json::parse(stream);
    int rows = 0;
    bool all_valid = true;
    for (const auto& [name, row] : catalog.items()) {
        if (!row.is_object() || row.value("recipe", "") != "loom") continue;
        ++rows;
        const auto info = lemon::models::parse_registry_row(name, row,
                                                            lemon::models::RegistryRowRules{});
        const bool valid = info.has_value() &&
                           loom::ops()->validate_registration_checkpoint(info->checkpoint()).empty() &&
                           info->checkpoint().rfind("loom-fun/", 0) == 0;
        if (!valid) std::printf("  invalid catalog row: %s\n", name.c_str());
        all_valid = all_valid && valid;
    }
    check("The catalog ships the loom-fun bundles", rows >= 51);
    check("Every loom row names a loom-fun bundle repository", all_valid);
    check("A variant suffix is not a bundle",
          !loom::ops()->validate_registration_checkpoint("loom-fun/x-loom:Q4_K_M").empty());
}

}  // namespace

int main() {
    try {
        check_bundle_plan();
        check_paths();
        check_launch_contract();
        check_device_targets();
        check_request_contract();
        check_readiness_contract();
        check_startup_failure_detail();
        check_card_metadata();
        check_installer_contract();
        check_descriptor_contract();
        check_catalog_contract();
    } catch (const std::exception& error) {
        check(std::string("unexpected exception: ") + error.what(), false);
    }

    if (failures == 0) {
        std::printf("\nAll loom contract checks passed.\n");
        return 0;
    }
    std::printf("\n%d loom contract check(s) failed.\n", failures);
    return 1;
}
