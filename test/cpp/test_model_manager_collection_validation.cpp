// Tests for ModelManager collection registration validation relevant to
// collection.router policy loading (#2383), and for the aggregate
// max_context_window computed for collection models at cache-build time.

#include "lemon/config_file.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/utils/path_utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using lemon::ModelManager;
using lemon::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static fs::path make_temp_dir() {
    fs::path dir = fs::temp_directory_path();
    dir /= "model_manager_collection_validation_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    fs::create_directories(dir);
    return dir;
}

static json component_def(const std::string& name) {
    return json{
        {"model_name", name},
        {"recipe", "llamacpp"},
        {"checkpoint", "example/" + name + ":Q4_K_M"},
    };
}

static json valid_router_collection() {
    return json{
        {"model_name", "user.RouterKit"},
        {"version", "1"},
        {"recipe", "collection.router"},
        {"components", {"local", "remote", "pii-detector"}},
        {"models", {
            component_def("local"),
            component_def("remote"),
            component_def("pii-detector"),
        }},
        {"routing", {
            {"candidates", {"local", "remote"}},
            {"default_model", "local"},
            {"classifiers", {{
                {"id", "pii"},
                {"type", "classifier"},
                {"model", "pii-detector"},
                {"labels", {"PII", "NO_PII"}},
                {"default_label", "PII"},
                {"on_error", "match_true"},
            }}},
            {"rules", {{
                {"id", "private-local"},
                {"match", {{"classifier", "pii"}, {"min_score", 0.5}}},
                {"route_to", "local"},
                {"outputs", {{"verdict", "warn"}}},
            }, {
                {"id", "code-remote"},
                {"match", {{"keywords_any", {"def ", "stack trace"}}}},
                {"route_to", "remote"},
            }}},
        }},
    };
}

static bool error_contains(const std::optional<std::string>& error,
                           const std::string& needle) {
    return error.has_value() && error->find(needle) != std::string::npos;
}

static void test_accepts_valid_router_policy(ModelManager& manager) {
    json doc = valid_router_collection();
    auto err = manager.validate_collection_request("user.RouterKit", doc);
    check("valid collection.router request passes validation", !err.has_value());
}

static void test_rejects_bad_routing(ModelManager& manager) {
    json doc = valid_router_collection();
    doc["routing"]["rules"][0]["route_to"] = "missing";
    auto err = manager.validate_collection_request("user.RouterKit", doc);
    check("invalid route_to in routing is rejected",
          error_contains(err, "Invalid collection.router routing policy") &&
          error_contains(err, "not declared in collection.components"));

    json bad_band = valid_router_collection();
    bad_band["routing"]["rules"][0]["match"]["min_score"] = 0.9;
    bad_band["routing"]["rules"][0]["match"]["max_score"] = 0.1;
    err = manager.validate_collection_request("user.RouterKit", bad_band);
    check("invalid score band is rejected",
          error_contains(err, "min_score greater than max_score"));

    json bad_regex = valid_router_collection();
    bad_regex["routing"]["rules"][1]["match"] = {{"regex", "(a+)+"}};
    err = manager.validate_collection_request("user.RouterKit", bad_regex);
    check("compile-time leaf validation rejects catastrophic regex",
          error_contains(err, "catastrophic backtracking"));
}

// Build a minimal router whose single classifier of `type` is backed by the
// given inline model definition. `local` is the only candidate.
static json router_with_classifier(const std::string& type,
                                   const json& classifier_model_def) {
    const std::string cmodel = classifier_model_def.value("model_name", "clf");
    json classifier = {{"id", "clf"}, {"type", type}, {"model", cmodel}};
    json match;
    if (type == "semantic_similarity") {
        classifier["reference_phrases"] = {{"coding", {"write code"}}};
        match = {{"classifier", "clf"}, {"label", "coding"}, {"min_score", 0.5}};
    } else {
        classifier["labels"] = {"A", "B"};
        classifier["default_label"] = "A";
        match = {{"classifier", "clf"}, {"min_score", 0.5}};
    }
    return json{
        {"model_name", "user.RouterKit"},
        {"version", "1"},
        {"recipe", "collection.router"},
        {"components", {"local", cmodel}},
        {"models", {component_def("local"), classifier_model_def}},
        {"routing", {
            {"candidates", {"local"}},
            {"default_model", "local"},
            {"classifiers", {classifier}},
            {"rules", {{
                {"id", "r"},
                {"match", match},
                {"route_to", "local"},
            }}},
        }},
    };
}

// The inline type resolver must mirror registration's normalization, not just
// read explicit labels: a label-less definition still picks up legacy flags and
// the backend's default labels.
static void test_inline_capability_matches_registration(ModelManager& manager) {
    // Label-less sd-cpp -> IMAGE at registration, so it cannot be a classifier.
    json sd_clf = router_with_classifier(
        "classifier",
        json{{"model_name", "img"}, {"recipe", "sd-cpp"}, {"checkpoint", "example/img"}});
    auto err = manager.validate_collection_request("user.RouterKit", sd_clf);
    check("label-less sd-cpp rejected as classifier (backend default label 'image')",
          error_contains(err, "cannot serve as a classifier"));

    // Legacy `embedding: true` -> EMBEDDING, valid for semantic_similarity.
    json emb_sem = router_with_classifier(
        "semantic_similarity",
        json{{"model_name", "emb"}, {"recipe", "llamacpp"},
             {"checkpoint", "example/emb:Q4_K_M"}, {"embedding", true}});
    err = manager.validate_collection_request("user.RouterKit", emb_sem);
    check("legacy embedding:true accepted for semantic_similarity", !err.has_value());

    // Label-less regular llamacpp still defaults to LLM, valid as a classifier.
    json llm_clf = router_with_classifier(
        "classifier",
        json{{"model_name", "reg"}, {"recipe", "llamacpp"},
             {"checkpoint", "example/reg:Q4_K_M"}});
    err = manager.validate_collection_request("user.RouterKit", llm_clf);
    check("label-less llamacpp still defaults to LLM (valid classifier)", !err.has_value());
}

// A chat-indicator label (reasoning/vision/…) must not promote a non-chat
// backend to LLM. The backend's deployment capability wins, at both the model
// type (which drives runtime routing) and collection.router validation.
static void test_backend_capability_over_chat_indicator(ModelManager& manager) {
    // onnxruntime is a classification backend: reasoning:true stays CLASSIFICATION,
    // so run_classifier routes to /classify, not the (unsupported) chat path.
    manager.register_user_model(
        "user.GuardX",
        json{{"model_name", "user.GuardX"}, {"recipe", "onnxruntime"},
             {"checkpoint", "example/guard"}, {"reasoning", true}});
    check("onnxruntime + reasoning:true is CLASSIFICATION, not LLM",
          manager.get_model_info("user.GuardX").type == lemon::ModelType::CLASSIFICATION);

    // sd-cpp is an image backend: vision:true stays IMAGE, not LLM.
    manager.register_user_model(
        "user.ImgX",
        json{{"model_name", "user.ImgX"}, {"recipe", "sd-cpp"},
             {"checkpoint", "example/img"}, {"vision", true}});
    check("sd-cpp + vision:true is IMAGE, not LLM",
          manager.get_model_info("user.ImgX").type == lemon::ModelType::IMAGE);

    // …and the same models used as router classifiers resolve accordingly:
    // onnxruntime is accepted (CLASSIFICATION), sd-cpp is rejected (IMAGE).
    json onnx_reason = router_with_classifier(
        "classifier",
        json{{"model_name", "guard"}, {"recipe", "onnxruntime"},
             {"checkpoint", "example/guard"}, {"reasoning", true}});
    check("onnxruntime + reasoning:true accepted as classifier (CLASSIFICATION)",
          !manager.validate_collection_request("user.RouterKit", onnx_reason).has_value());

    json sd_vision = router_with_classifier(
        "classifier",
        json{{"model_name", "img"}, {"recipe", "sd-cpp"},
             {"checkpoint", "example/img"}, {"vision", true}});
    check("sd-cpp + vision:true rejected as classifier (still IMAGE)",
          error_contains(manager.validate_collection_request("user.RouterKit", sd_vision),
                         "cannot serve as a classifier"));

    // kokoro only does TTS but has no explicit labels; its default label must
    // still keep a label-less kokoro model out of the classifier path.
    json kokoro_clf = router_with_classifier(
        "classifier",
        json{{"model_name", "voice"}, {"recipe", "kokoro"}, {"checkpoint", "example/voice"}});
    check("label-less kokoro rejected as classifier (default label 'tts')",
          error_contains(manager.validate_collection_request("user.RouterKit", kokoro_clf),
                         "cannot serve as a classifier"));

    // The inverse: /v1/classify is served only by onnxruntime. A `classification`
    // label on llamacpp (which cannot classify) must NOT type it CLASSIFICATION,
    // or run_classifier would call Router::classify() and fail. It stays LLM and
    // is accepted as an LLM-as-classifier via the chat path.
    manager.register_user_model(
        "user.LlamaClf",
        json{{"model_name", "user.LlamaClf"}, {"recipe", "llamacpp"},
             {"checkpoint", "example/x:Q4_K_M"}, {"labels", {"classification"}}});
    check("llamacpp + labels:[classification] stays LLM, not CLASSIFICATION",
          manager.get_model_info("user.LlamaClf").type == lemon::ModelType::LLM);

    json llama_clf_label = router_with_classifier(
        "classifier",
        json{{"model_name", "xclf"}, {"recipe", "llamacpp"},
             {"checkpoint", "example/xclf:Q4_K_M"}, {"labels", {"classification"}}});
    check("llamacpp + labels:[classification] accepted as classifier via LLM chat path",
          !manager.validate_collection_request("user.RouterKit", llama_clf_label).has_value());
}

static void test_register_preserves_routing(ModelManager& manager) {
    json doc = valid_router_collection();
    manager.register_user_model("user.RouterKit", doc);
    auto info = manager.get_model_info("user.RouterKit");
    auto it = info.extras.find("routing");
    check("registered router collection preserves routing in ModelInfo extras",
          it != info.extras.end() && it->second == doc["routing"]);
}

static void write_fake_gguf(const fs::path& path, uint32_t context_length) {
    std::ofstream out(path, std::ios::binary);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    auto w32 = [&](uint32_t v) {
        uint8_t buf[4] = {
            static_cast<uint8_t>(v),
            static_cast<uint8_t>(v >> 8),
            static_cast<uint8_t>(v >> 16),
            static_cast<uint8_t>(v >> 24),
        };
        out.write(reinterpret_cast<const char*>(buf), 4);
    };
    auto w64 = [&](uint64_t v) {
        uint8_t buf[8] = {
            static_cast<uint8_t>(v),
            static_cast<uint8_t>(v >> 8),
            static_cast<uint8_t>(v >> 16),
            static_cast<uint8_t>(v >> 24),
            static_cast<uint8_t>(v >> 32),
            static_cast<uint8_t>(v >> 40),
            static_cast<uint8_t>(v >> 48),
            static_cast<uint8_t>(v >> 56),
        };
        out.write(reinterpret_cast<const char*>(buf), 8);
    };
    auto wstr = [&](const std::string& s) {
        w64(s.size());
        out.write(s.data(), static_cast<std::streamsize>(s.size()));
    };
    out.write("GGUF", 4);
    w32(3);  // version
    w64(0);  // tensor count
    w64(2);  // kv count
    wstr("general.architecture");
    w32(8);  // string
    wstr("llama");
    wstr("llama.context_length");
    w32(4);  // uint32
    w32(context_length);
}

static void register_local_gguf(ModelManager& manager, const std::string& name,
                                const fs::path& temp, uint32_t context_length) {
    fs::path gguf = temp / (name + ".gguf");
    write_fake_gguf(gguf, context_length);
    json def = {
        {"recipe", "llamacpp"},
        {"checkpoint", lemon::utils::path_to_utf8(gguf)},
        {"source", "local_path"},
    };
    manager.register_user_model("user." + name, def);
}

static void test_collection_aggregate_context_window(ModelManager& manager,
                                                     const fs::path& temp) {
    register_local_gguf(manager, "ctx-large", temp, 32768);
    register_local_gguf(manager, "ctx-small", temp, 8192);
    register_local_gguf(manager, "ctx-classifier", temp, 512);

    json router = {
        {"version", "1"},
        {"recipe", "collection.router"},
        {"components", {"ctx-large", "ctx-small", "ctx-classifier"}},
        {"routing", {
            {"candidates", {"ctx-large", "ctx-small"}},
            {"default_model", "ctx-large"},
            {"classifiers", {{
                {"id", "pii"},
                {"type", "classifier"},
                {"model", "ctx-classifier"},
                {"labels", {"PII", "NO_PII"}},
                {"default_label", "PII"},
                {"on_error", "match_true"},
            }}},
            {"rules", {{
                {"id", "private-small"},
                {"match", {{"classifier", "pii"}, {"min_score", 0.5}}},
                {"route_to", "ctx-small"},
            }}},
        }},
    };
    manager.register_user_model("user.CtxRouter", router);

    auto large = manager.get_model_info("user.ctx-large");
    check("local gguf component resolves its own context window",
          large.max_context_window == 32768);

    auto router_info = manager.get_model_info("user.CtxRouter");
    check("collection.router is marked downloaded when bare component names resolve",
          router_info.downloaded);
    check("collection.router aggregates min candidate context window",
          router_info.max_context_window == 8192);

    json omni = {
        {"recipe", "collection.omni"},
        {"components", {"ctx-large", "ctx-small", "ctx-classifier", "ctx-absent"}},
    };
    manager.register_user_model("user.CtxOmni", omni);
    auto omni_info = manager.get_model_info("user.CtxOmni");
    check("collection.omni aggregates min component context window, skipping unknowns",
          omni_info.max_context_window == 512);

    manager.update_model_in_cache("user.ctx-small", false);
    router_info = manager.get_model_info("user.CtxRouter");
    check("collection.router download state refreshes when a canonical component changes",
          !router_info.downloaded);
    check("collection.router context window refreshes when a canonical component changes",
          router_info.max_context_window == 32768);
}

int main() {
    fs::path temp = make_temp_dir();
    lemon::utils::set_cache_dir(temp.string());

    // Disable hardware-based model filtering so the llamacpp component models
    // used by the aggregate-context-window test survive cache build on any
    // machine this test runs on.
    json config = lemon::ConfigFile::load(temp.string());
    config["disable_model_filtering"] = true;
    auto runtime_config = std::make_unique<lemon::RuntimeConfig>(config);
    lemon::RuntimeConfig::set_global(runtime_config.get());

    ModelManager manager;
    test_accepts_valid_router_policy(manager);
    test_rejects_bad_routing(manager);
    test_inline_capability_matches_registration(manager);
    test_backend_capability_over_chat_indicator(manager);
    test_register_preserves_routing(manager);
    test_collection_aggregate_context_window(manager, temp);

    lemon::RuntimeConfig::set_global(nullptr);
    fs::remove_all(temp);

    if (g_failures == 0) {
        std::printf("All model manager collection validation tests passed.\n");
    } else {
        std::printf("%d model manager collection validation test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
