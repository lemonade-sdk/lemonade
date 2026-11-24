// Proves the loom recipe's bundle planning, launch, request shaping, readiness,
// install, catalog and cache layout contracts without a GPU, a network or a
// running loom-serve.

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/loom/loom.h"
#include "lemon/backends/loom/loom_bundle.h"
#include "lemon/backends/loom/loom_server.h"
#include "lemon/models/json_catalog.h"
#include "lemon/utils/path_utils.h"

#include "support/test_state_isolation.h"

#include <nlohmann/json.hpp>

#include <chrono>
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
    check("Plan lists one file per pinned parameter", plan.origin_files.size() == 2);
    if (plan.origin_files.size() == 2) {
        const auto& model = plan.origin_files[0].path == "parameter/model.gguf"
                                ? plan.origin_files[0]
                                : plan.origin_files[1];
        check("A parameter keeps its card-relative path, repository, revision and file",
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

    json url_origin = kCard;
    url_origin["origins"]["drafter"]["kind"] = "url";
    check("Non-Hugging-Face origins are rejected",
          throws_exception([&] { (void)loom::plan_bundle(url_origin, kLock); }));

    json materialized = kCard;
    materialized["origins"]["document"] = {{"kind", "url"}};
    materialized["programs"]["source"]["inputs"][0]["origin"] = "@document/program/prepare.loom";
    json materialized_lock = kLock;
    materialized_lock["files"].push_back({{"origin", "@document/program/prepare.loom"},
                                           {"size", 123u}, {"sha256", "digest"}});
    const auto installed = loom::plan_bundle(materialized, materialized_lock);
    check("Installed document files are verified locally",
          installed.origin_files.size() == 2 && installed.locked_files.size() == 2 &&
              installed.locked_files[0].path == "program/prepare.loom");
    check("Installed document files still require a locked digest",
          throws_exception([&] { (void)loom::plan_bundle(materialized, kLock); }));

    json missing_digest = kLock;
    missing_digest["files"].erase(1);
    check("A parameter without a locked digest is rejected",
          throws_exception([&] { (void)loom::plan_bundle(kCard, missing_digest); }));

    json escaping = kCard;
    escaping["variants"]["gguf"]["services"]["chat"]["parameters"]["model"]["files"][0]["path"] =
        "../outside.gguf";
    check("A parameter placed outside the card's folder is rejected",
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
    check("An org/name repository id is accepted",
          !throws_exception([] { loom::validate_repo_id("loom-fun/unsloth-Qwen3.5-0.8B-loom"); }));
    for (const std::string repo : {"noslash", "a/b/c", "/name", "org/", "org/..", "org/a b"}) {
        check("Repository id is rejected: '" + repo + "'",
              throws_exception([&] { loom::validate_repo_id(repo); }));
    }
}

void check_program_files() {
    json card = kCard;
    card["programs"]["source"]["inputs"].push_back("program/control.loom");
    card["programs"]["source"]["libraries"] = {"program/llm.loom"};
    check("Program sources, their data files and libraries are listed",
          loom::program_files(card) ==
              std::vector<std::string>{"program/prepare.loom", "weights.loom",
                                       "program/control.loom", "program/llm.loom"});
    check("A card without programs lists none", loom::program_files(json::object()).empty());

    json escaping = kCard;
    escaping["programs"]["source"]["inputs"][0]["data"] = {"../weights.loom"};
    check("A program data file outside the card's folder is rejected",
          throws_exception([&] { (void)loom::program_files(escaping); }));
}

void check_launch_contract() {
    const std::string device = "--device=amdgpu://";
    const std::vector<std::string> expected = {
        "--model=/run/model.json",
        "--listen=127.0.0.1:14123",
        device + "?completion_spin_ns=123",
        "--preset=vision-mtp",
        "--memory_bytes=57000000000",
        "--workers=4",
    };
    check("Launch argv binds loopback and appends benign custom flags",
          loom::build_server_argv("/run", 14123, "vision-mtp", {"--memory_bytes=57000000000"},
                                  "--workers=4", "completion_spin_ns=123") == expected);
    check("The legacy runtime still receives its parameter mode",
          loom::build_server_argv("/run", 1, "", {"--pool_backing=fixed", loom::kPreloadFlag},
                                  "", "", true) ==
              std::vector<std::string>({"--model=/run/model.json", "--listen=127.0.0.1:1", device,
                                        "--pool_backing=fixed", "--parameter_mode=preload"}));
    check("The 0.3 launch drops obsolete parameter modes from both input sources",
          loom::build_server_argv("/run", 1, "", {"--pool_backing=fixed", loom::kPreloadFlag},
                                  "--parameter_mode=mmap --parameter_mode=direct", "completion_spin_ns=123") ==
              std::vector<std::string>({"--model=/run/model.json", "--listen=127.0.0.1:1",
                                        device + "?completion_spin_ns=123",
                                        "--pool_backing=fixed", "--parameter_mode=direct"}));
    check("Launch argv omits an unset preset and serve flags",
          loom::build_server_argv("/run", 1, "", {}, "") ==
              std::vector<std::string>({"--model=/run/model.json", "--listen=127.0.0.1:1", device}));
    check("Serve flags are passed through in order",
          loom::build_server_argv("/run", 1, "", {"--pool_backing=fixed", loom::kPreloadFlag}, "") ==
              std::vector<std::string>({"--model=/run/model.json", "--listen=127.0.0.1:1", device,
                                        "--pool_backing=fixed", "--parameter_mode=preload"}));
    check("A custom device and memory size follow the defaults, so they win",
          loom::build_server_argv("/run", 1, "", {}, "--device=amdgpu://1 --memory_bytes=16gib", "completion_spin_ns=123") ==
              std::vector<std::string>({"--model=/run/model.json", "--listen=127.0.0.1:1", device + "?completion_spin_ns=123",
                                        "--device=amdgpu://1", "--memory_bytes=16gib"}));

    for (const std::string custom : {"--listen=0.0.0.0:80", "--model=other.json",
                                     "--devices=d.json", "--preset=x",
                                     "--flagfile=f", "--listen 0.0.0.0:80"}) {
        check("Reserved custom argument is rejected: " + custom, throws_exception([&] {
                  (void)loom::build_server_argv("/run", 1, "", {}, custom);
              }));
    }
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
    check("A gfx1201 ISA with 16 GB runs the gfx1201-16gb build",
          loom::device_target("gfx1201", "120001", 15.9) == loom::kGfx120116gbTarget);
    check("A gfx1201 ISA with 14 GiB is the 16 GB class",
          loom::device_target("gfx1201", "120001", 14.0) == loom::kGfx120116gbTarget);
    check("A gfx1201 ISA with 30 GiB is the 32 GB class",
          loom::device_target("gfx1201", "120001", 30.0) == loom::kRadeonR9700Target);
    check("A gfx1201 ISA below 14 GiB has no build",
          loom::device_target("gfx1201", "120001", 8.0).empty());
    check("A Windows gfx120X family with 16 GB runs the gfx1201-16gb build",
          loom::device_target("gfx120X", "AMD Radeon RX 9070 XT", 16.0) == loom::kGfx120116gbTarget);
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

    const json raw_response = {{"id", "loom-1"}, {"model", "qwen3.8-27b"}, {"choices", json::array()}};
    check("Response model is normalized to the requested Lemonade model id",
          loom::normalize_chat_response(raw_response, {{"model", "Qwen3.8-27B-Loom"}}, "fallback")["model"] ==
              "Qwen3.8-27B-Loom");
    check("Response model falls back when request omits model",
          loom::normalize_chat_response(raw_response, json::object(), "Qwen3.8-27B-Loom")["model"] ==
              "Qwen3.8-27B-Loom");
    check("Error responses without model stay untouched",
          !loom::normalize_chat_response({{"error", {{"message", "bad"}}}}, {{"model", "Qwen3.8-27B-Loom"}}, "fallback")
               .contains("model"));
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
    const json cards = json::parse(R"({"devices":{
        "strix-halo":{"quick_start":{"preset":"chat","device":"amdgpu://?completion_spin_ns=4294967295",
                                     "args":["--pool_backing=fixed","--parameter_mode=preload"]}},
        "radeon-r9700":{"quick_start":{"preset":"chat-r9700","device":"amdgpu://?completion_spin_ns=4294967295",
                                       "args":["--pool_backing=fixed","--parameter_mode=preload"]}},
        "gfx1201-16gb":{"quick_start":{"preset":null,"device":"amdgpu://",
                                       "args":["--memory_bytes=15000000000","--state_slab_bytes=4mib"]}}}})");
    const loom::QuickStart strix = loom::quick_start(cards, loom::kStrixHaloTarget);
    check("A GPU serves the preset, device query and flags its card records",
          strix.preset == "chat" && strix.device_query == "completion_spin_ns=4294967295" &&
              strix.args == Args({"--pool_backing=fixed", "--parameter_mode=preload"}));
    check("The R9700 serves its own recorded preset",
          loom::quick_start(cards, loom::kRadeonR9700Target).preset == "chat-r9700");
    const loom::QuickStart small = loom::quick_start(cards, loom::kGfx120116gbTarget);
    check("A card without a preset or device query leaves both empty",
          small.preset.empty() && small.device_query.empty());
    check("Recorded flags are served as they are where the host preloads",
          loom::serve_args(strix, true, "") == Args({"--pool_backing=fixed", "--parameter_mode=preload"}));
    check("No preload where the host has no memlock problem",
          loom::serve_args(strix, false, "") == Args({"--pool_backing=fixed"}));
    check("A custom size drops the recorded fixed pool",
          loom::serve_args(strix, true, "--memory_bytes=16gib") ==
              Args({"--parameter_mode=preload"}));
    check("A size the user asks for replaces the recorded one",
          loom::serve_args(small, true, "--memory_bytes 8gib") == Args({"--state_slab_bytes=4mib"}));
    check("Other custom flags keep the recorded size",
          loom::serve_args(small, true, "--workers=4") == small.args);
    check("Preload is not added when the card omits it",
          loom::serve_args(small, true, "") == small.args);
    const json flash = json::parse(R"({"devices":{"strix-halo":{"quick_start":{
        "preset":"vision-mtp","device":"amdgpu://?completion_spin_ns=4294967295",
        "args":["--workers=8"]}}}})");
    const loom::QuickStart flash_start = loom::quick_start(flash, loom::kStrixHaloTarget);
    check("The Flash Quick start keeps its device query, workers and elastic default",
          flash_start.preset == "vision-mtp" &&
              flash_start.device_query == "completion_spin_ns=4294967295" &&
              loom::serve_args(flash_start, false, "") == Args({"--workers=8"}));
    check("A bundle reports whether it records a Quick start for a GPU",
          loom::records_quick_start(cards, loom::kStrixHaloTarget) &&
              !loom::records_quick_start(cards, "gfx1150") &&
              !loom::records_quick_start(json::object(), loom::kStrixHaloTarget) &&
              !loom::records_quick_start(json(), loom::kStrixHaloTarget));
    check("A GPU without a recorded Quick start is refused",
          throws_exception([&] { (void)loom::quick_start(cards, "gfx1150"); }) &&
              throws_exception([&] { (void)loom::quick_start(json::object(), loom::kStrixHaloTarget); }));
    for (const std::string bad :
         {"\"--listen=0.0.0.0:18080\"", "\"--model=other.json\"", "\"--device=amdgpu://1\"",
          "\"--devices=d.json\"", "\"--flagfile=f\"", "\"--preset=x\"", "\"--listen\"",
          "\"0.0.0.0:80\"", "\"--memory_bytes=\"", "\"--memory_bytes=1 --listen=0.0.0.0:1\"",
          "\"--workers=0\"", "\"--workers=9\"", "\"--workers=8 --listen=0.0.0.0:1\"", "1"}) {
        const json info_with_bad = json::parse(
            R"({"devices":{"strix-halo":{"quick_start":{"preset":"chat","device":"amdgpu://","args":["--memory_bytes=1",)" +
            bad + "]}}}}");
        check("A recorded serve argument outside the allowlist is rejected: " + bad,
              throws_exception([&] { (void)loom::quick_start(info_with_bad, loom::kStrixHaloTarget); }));
    }
    for (const std::string bad : {R"("device":"file:///x","preset":"chat")", R"("device":"amdgpu://?a=1 --listen","preset":"chat")",
                                  R"("device":"amdgpu://","preset":"a b")", R"("device":"amdgpu://","preset":1)"}) {
        const json info_with_bad = json::parse(
            R"({"devices":{"strix-halo":{"quick_start":{)" + bad + R"(,"args":[]}}}})");
        check("A recorded device or preset outside the allowlist is rejected: " + bad,
              throws_exception([&] { (void)loom::quick_start(info_with_bad, loom::kStrixHaloTarget); }));
    }
    json r9700_card = kCard;
    r9700_card["presets"]["chat-r9700"] = {{"execution", {{"context_capacity", 131072}, {"max_tokens", 8192}}}};
    r9700_card["presets"]["chat-16gb"] = {{"execution", {{"context_capacity", 65536}, {"max_tokens", 2048}}}};
    const auto strix_limits = loom::preset_limits(r9700_card, strix.preset);
    check("Strix Halo reads the base preset's context capacity and max tokens",
          strix_limits.context_capacity == 262144 && strix_limits.max_tokens == 4096);
    const auto r9700_limits = loom::preset_limits(r9700_card, loom::quick_start(cards, loom::kRadeonR9700Target).preset);
    check("R9700 reads the -r9700 preset's context capacity and max tokens",
          r9700_limits.context_capacity == 131072 && r9700_limits.max_tokens == 8192);
    const auto gfx16_limits = loom::preset_limits(r9700_card, "chat-16gb");
    check("16 GB class reads the -16gb preset's context capacity and max tokens",
          gfx16_limits.context_capacity == 65536 && gfx16_limits.max_tokens == 2048);

    json vision_card = kCard;
    vision_card["presets"] = {
        {"vision", {{"execution", {{"context_capacity", 262144}, {"max_tokens", 4096}}}}},
        {"vision-r9700", {{"execution", {{"context_capacity", 131072}, {"max_tokens", 4096}}}}},
        {"vision-16gb", {{"execution", {{"context_capacity", 32768}, {"max_tokens", 2048}}}}},
    };
    check("Non-chat base preset keeps its own capacity on Strix Halo instead of last alphabetical variant",
          loom::preset_limits(vision_card, "vision").context_capacity == 262144);
    check("Non-chat base preset resolves its -16gb variant",
          loom::preset_limits(vision_card, "vision-16gb").context_capacity == 32768);
    vision_card["presets"]["deployment"] = vision_card["presets"]["vision-16gb"];
    json custom_cards = cards;
    custom_cards["devices"]["strix-halo"]["quick_start"]["preset"] = "deployment";
    check("Metadata follows a recorded preset without a GPU naming convention",
          loom::preset_limits(vision_card, loom::quick_start(custom_cards, loom::kStrixHaloTarget).preset)
                  .context_capacity == 32768);
    check("An unknown preset does not report another preset's limits",
          loom::preset_limits(vision_card, "missing").context_capacity == 0);
    check("An unambiguous card can report limits without a named preset",
          loom::preset_limits(kCard).context_capacity == 262144);
    check("Verified features become catalog labels",
          loom::suggested_labels(info, "Qwen3.6-27B-Loom") ==
              std::vector<std::string>({"chat", "vision", "tool-calling", "reasoning", "dflash"}));
    check("Search names drop the GGUF publisher and the -loom suffix",
          loom::suggested_name("loom-fun/ggml-org-Qwen3.6-27B-loom", kCard) == "Qwen3.6-27B-Loom");
    check("Bundle size sums the locked files",
          loom::locked_bytes(kLock) == 19095766304ull + 1033066592ull + 9832586ull);
}

void check_installer_contract() {
#if defined(_WIN32) || (defined(__linux__) && (defined(__x86_64__) || defined(__amd64__)))
    const auto current = LoomServer::get_install_params("hrx", "v0.3.0");
#if defined(_WIN32)
    check("Loom 0.3.0 uses the Windows wheel with the C++ importer",
          current.filename == "hrx_loom-0.3.0-py3-none-win_amd64.whl");
    check("The Windows wheel installs its executable and runtime DLLs together",
          current.archive_subdir == "hrx_loom-0.3.0.data/scripts" &&
              current.required_install_files == std::vector<std::string>{"iree.dll", "loomc.dll"});
#else
    check("Loom 0.3.0 uses the Linux wheel with the C++ importer",
          current.filename == "hrx_loom-0.3.0-py3-none-manylinux_2_39_x86_64.whl");
    check("The Linux wheel installs its executable and native libraries together",
          current.archive_subdir == "hrx_loom-0.3.0.data/scripts" &&
              current.required_install_files == std::vector<std::string>{"libiree.so", "libloomc.so"});
#endif
#endif

#if defined(_WIN32)
    const auto params = LoomServer::get_install_params("hrx", "v0.2.0");
    check("Windows installs the win_amd64 wheel",
          params.filename == "hrx_loom-0.2.0-py3-none-win_amd64.whl");
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    const auto params = LoomServer::get_install_params("hrx", "v0.2.0");
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
          support.size() == 1 && support[0].backend == "hrx" &&
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
          versions["loom"]["hrx"] == loom::kBundleRevision);
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

// The Hugging Face cache loom-fetch leaves behind: the bundle repository's files
// in a snapshot, each origin file in its own repository's snapshot at the pinned
// revision, and refs/main on whatever revision `hf` last resolved.
class FixtureHub {
public:
    static constexpr const char* kBundleRepo = "loom-fun/fixture-bundle-loom";
    static constexpr const char* kSnapshot = "1111111111111111111111111111111111111111";
    static constexpr const char* kGgufRevision = "2222222222222222222222222222222222222222";
    static constexpr const char* kGgufSha256 =
        "3333333333333333333333333333333333333333333333333333333333333333";

    explicit FixtureHub(const std::string& name) : root_(lemon::test::make_scratch_dir(name)) {
        lemon::utils::set_models_dir(root_.string());
    }

    fs::path bundle_cache() const { return root_ / "models--loom-fun--fixture-bundle-loom"; }
    fs::path gguf_cache() const { return root_ / "models--fixture-org--Fixture-GGUF"; }
    fs::path snapshot(const std::string& revision = kSnapshot) const {
        return bundle_cache() / "snapshots" / revision;
    }

    static json card() {
        return json::parse(R"({
          "model": {"id": "Fixture/Model"},
          "programs": {"source": {
            "inputs": [{"path": "program/prepare.loom", "data": ["weights.loom"]}],
            "libraries": ["program/llm.loom"]}},
          "variants": {"gguf": {"services": {"chat": {
            "parameters": {"model": {"files": [{"path": "parameter/model.gguf", "format": "gguf",
                                                 "origin": "@checkpoint/model.gguf"}]}},
            "tokenizers": {"text": "tokenizer.json"}}}}},
          "presets": {"chat": {"execution": {"context_capacity": 4096}}},
          "origins": {"checkpoint": {"kind": "hf", "repository": "fixture-org/Fixture-GGUF",
                                     "revision": "2222222222222222222222222222222222222222"}}
        })");
    }

    static json lock() {
        return json{{"schema_version", 2},
                    {"files",
                     {{{"origin", "@checkpoint/model.gguf"}, {"size", 4}, {"sha256", kGgufSha256}},
                      {{"origin", "tokenizer.json"},
                       {"size", 2},
                       {"sha256", std::string(64, '4')}}}}};
    }

    // The files loom-fetch's `hf` downloads land in a bundle snapshot: no parameter/.
    void write_bundle(const std::string& revision = kSnapshot) const {
        write(snapshot(revision) / "model.json", card().dump());
        write(snapshot(revision) / "model.json.lock", lock().dump());
        write(snapshot(revision) / "tokenizer.json", "{}");
        write(snapshot(revision) / "weights.loom", "w");
        write(snapshot(revision) / "program" / "prepare.loom", "p");
        write(snapshot(revision) / "program" / "llm.loom", "l");
    }

    void write_card_only(const std::string& revision = kSnapshot) const {
        write(snapshot(revision) / "model.json", card().dump());
        write(snapshot(revision) / "model.json.lock", lock().dump());
        write(snapshot(revision) / "info.json", "{}");
    }

    void write_gguf_snapshot() const {
        write(gguf_cache() / "snapshots" / kGgufRevision / "model.gguf", "gguf");
    }

    void write_ref_main(const std::string& revision) const {
        write(bundle_cache() / "refs" / "main", revision);
    }

    static void write(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    }

    static lemon::ModelInfo model() {
        lemon::ModelInfo info;
        info.model_name = "Fixture-Loom";
        info.recipe = "loom";
        info.checkpoints["main"] = kBundleRepo;
        return info;
    }

    static bool downloaded() {
        return loom::ops()->is_downloaded(model(), lemon::backends::BackendOpsContext{});
    }

    static fs::path resolved() {
        lemon::backends::CheckpointResolveContext ctx;
        ctx.checkpoint = kBundleRepo;
        ctx.repo_id = kBundleRepo;
        ctx.type = "main";
        return fs::path(loom::ops()->resolve_checkpoint_path(model(), ctx));
    }

private:
    fs::path root_;
};

bool same_path(const fs::path& a, const fs::path& b) {
    return !a.empty() && a.lexically_normal() == b.lexically_normal();
}

void check_cache_layout() {
    {
        FixtureHub hub("loom_fetch_layout");
        hub.write_bundle();
        hub.write_gguf_snapshot();
        hub.write_ref_main(std::string(40, '9'));
        check("A loom-fetch bundle whose GGUF sits in its own repository is downloaded",
              FixtureHub::downloaded());
        check("It resolves to the complete snapshot, past a refs/main with no snapshot",
              same_path(FixtureHub::resolved(), hub.snapshot()));
    }
    {
        FixtureHub hub("loom_card_only");
        hub.write_card_only();
        hub.write_gguf_snapshot();
        hub.write_ref_main(FixtureHub::kSnapshot);
        check("A snapshot holding only the card is not downloaded", !FixtureHub::downloaded());
        check("A snapshot holding only the card resolves to nothing", FixtureHub::resolved().empty());
    }
    {
        FixtureHub hub("loom_missing_library");
        hub.write_bundle();
        hub.write_gguf_snapshot();
        fs::remove(hub.snapshot() / "program" / "llm.loom");
        check("A snapshot missing a program library is not downloaded", !FixtureHub::downloaded());
    }
    {
        FixtureHub hub("loom_missing_data");
        hub.write_bundle();
        hub.write_gguf_snapshot();
        fs::remove(hub.snapshot() / "weights.loom");
        check("A snapshot missing a program data file is not downloaded", !FixtureHub::downloaded());
    }
    {
        FixtureHub hub("loom_missing_tokenizer");
        hub.write_bundle();
        hub.write_gguf_snapshot();
        fs::remove(hub.snapshot() / "tokenizer.json");
        check("A snapshot missing a locked bundle file is not downloaded", !FixtureHub::downloaded());
    }
    {
        FixtureHub hub("loom_missing_gguf");
        hub.write_bundle();
        check("A bundle whose GGUF is cached nowhere is not downloaded", !FixtureHub::downloaded());
    }
    {
        FixtureHub hub("loom_resized_gguf");
        hub.write_bundle();
        FixtureHub::write(hub.gguf_cache() / "snapshots" / FixtureHub::kGgufRevision / "model.gguf",
                          "truncated");
        check("A cached GGUF of the wrong size does not count", !FixtureHub::downloaded());
    }
    {
        FixtureHub hub("loom_gguf_blob");
        hub.write_bundle();
        FixtureHub::write(hub.gguf_cache() / "blobs" / FixtureHub::kGgufSha256, "gguf");
        check("A GGUF found by its content-addressed blob counts", FixtureHub::downloaded());
    }
    {
        FixtureHub hub("loom_gguf_beside_card");
        hub.write_bundle();
        FixtureHub::write(hub.snapshot() / "parameter" / "model.gguf", "gguf");
        check("A GGUF already beside the card counts", FixtureHub::downloaded());
    }
    {
        FixtureHub hub("loom_old_marker");
        hub.write_bundle();
        hub.write_gguf_snapshot();
        FixtureHub::write(hub.snapshot() / loom::kCompleteMarker, json{{"loom", "v0.1.0"}}.dump());
        check("A snapshot Lemonade fetched for another Loom release is not downloaded",
              !FixtureHub::downloaded());
        FixtureHub::write(hub.snapshot() / loom::kCompleteMarker,
                          json{{"loom", loom::kBundleRevision}}.dump());
        check("A snapshot Lemonade fetched for this Loom release is downloaded",
              FixtureHub::downloaded());
    }
    {
        FixtureHub hub("loom_snapshot_choice");
        const std::string older(40, '5');
        const std::string newer(40, '6');
        hub.write_bundle(older);
        hub.write_bundle(newer);
        hub.write_gguf_snapshot();
        std::error_code ec;
        const auto now = fs::file_time_type::clock::now();
        fs::last_write_time(hub.snapshot(older), now - std::chrono::hours(48), ec);
        fs::last_write_time(hub.snapshot(newer), now, ec);
        check("Without refs/main the newest complete snapshot serves",
              same_path(FixtureHub::resolved(), hub.snapshot(newer)));
        hub.write_ref_main(older);
        check("refs/main's complete snapshot serves ahead of a newer one",
              same_path(FixtureHub::resolved(), hub.snapshot(older)));
    }
    {
        FixtureHub hub("loom_delete");
        hub.write_bundle();
        hub.write_gguf_snapshot();
        loom::ops()->remove_self_managed_model(FixtureHub::model());
        check("Deleting removes the bundle repository's cache", !fs::exists(hub.bundle_cache()));
        check("Deleting keeps the GGUF repository other tools share", fs::exists(hub.gguf_cache()));
    }
    lemon::utils::set_models_dir("");
}

}  // namespace

int main() {
    try {
        check_bundle_plan();
        check_paths();
        check_program_files();
        check_launch_contract();
        check_device_targets();
        check_request_contract();
        check_readiness_contract();
        check_startup_failure_detail();
        check_card_metadata();
        check_installer_contract();
        check_descriptor_contract();
        check_catalog_contract();
        check_cache_layout();
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
