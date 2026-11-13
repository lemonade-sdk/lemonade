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
    check("A card with a chat preset leaves the preset to loom-serve", plan.preset.empty());
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
          loom::build_server_argv("/run", 14123, "vision-mtp", "amdgpu://", "57000000000",
                                  "--workers=4") == expected);
    check("Launch argv omits unset preset and memory",
          loom::build_server_argv("/run", 1, "", "amdgpu://", "", "") ==
              std::vector<std::string>({"--model=/run/model.json", "--listen=127.0.0.1:1",
                                        "--device=amdgpu://"}));

    for (const std::string custom : {"--listen=0.0.0.0:80", "--model=other.json", "--device=task",
                                     "--devices=d.json", "--memory_bytes=1", "--preset=x",
                                     "--flagfile=f", "--listen 0.0.0.0:80"}) {
        check("Reserved custom argument is rejected: " + custom, throws_exception([&] {
                  (void)loom::build_server_argv("/run", 1, "", "amdgpu://", "", custom);
              }));
    }
    check("A device with whitespace is rejected", throws_exception([] {
              (void)loom::build_server_argv("/run", 1, "", "amdgpu:// --listen=0.0.0.0:1", "", "");
          }));
    check("An empty device is rejected",
          throws_exception([] { (void)loom::build_server_argv("/run", 1, "", "", "", ""); }));
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

void check_card_metadata() {
    const json info = json::parse(R"({"card":{"memory_bytes":57000000000,"features":{
        "image_input":{"status":"yes"},"tool_calling":{"status":"yes"},
        "thinking":{"status":"yes"},"audio":{"status":"n/a"},
        "speculative_decoding":{"status":"yes","value":{"method":"dflash"}}}}})");
    check("Card memory becomes the pool budget", loom::card_memory_bytes(info) == "57000000000");
    check("A card without memory leaves the budget to loom-serve",
          loom::card_memory_bytes(json::object()).empty());
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
    const auto params = LoomServer::get_install_params("amdgpu", "v0.1.0");
    check("Windows installs the win_amd64 wheel",
          params.filename == "hrx_loom-0.1.0-py3-none-win_amd64.whl");
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    const auto params = LoomServer::get_install_params("amdgpu", "v0.1.0");
    check("Linux installs the manylinux wheel",
          params.filename == "hrx_loom-0.1.0-py3-none-manylinux_2_39_x86_64.whl");
#endif
#if defined(_WIN32) || (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)))
    check("The wheel comes from the hrx-system release", params.repo == "ROCm/hrx-system");
    check("Only the wheel's scripts directory is installed",
          params.archive_subdir == "hrx_loom-0.1.0.data/scripts");
#endif
    check("Another backend is rejected",
          throws_exception([] { (void)LoomServer::get_install_params("rocm", "v0.1.0"); }));

    const fs::path staging = fs::temp_directory_path() / "lemonade-loom-contract.staging";
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::create_directories(staging / "hrx_loom-0.1.0.data" / "scripts");
    fs::create_directories(staging / "loom");
    std::ofstream(staging / "hrx_loom-0.1.0.data" / "scripts" / "loom-serve") << "binary";
    std::ofstream(staging / "loom" / "dsl.py") << "python";
    lemon::backends::BackendUtils::keep_archive_subdir(staging.string(),
                                                       "hrx_loom-0.1.0.data/scripts");
    check("The kept directory becomes the install root",
          fs::exists(staging / "loom-serve") && !fs::exists(staging / "loom") &&
              !fs::exists(staging / "hrx_loom-0.1.0.data"));
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
          support.size() == 1 && support[0].backend == "amdgpu" &&
              support[0].tier == lemon::BackendTier::Experimental);
    if (support.size() == 1) {
        const auto gpus = support[0].devices.find("amd_gpu");
        check("Loom supports only gfx1151",
              gpus != support[0].devices.end() && gpus->second.size() == 1 &&
                  gpus->second.count("gfx1151") == 1);
    }
    check("Loom pulls its own bundles", loom::descriptor.self_manages_downloads);
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
        check_request_contract();
        check_readiness_contract();
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
