#include <cstdio>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include <lemon/model_registry.h>
#include <lemon/runtime_config.h>

using json = nlohmann::json;
using lemon::RuntimeConfig;
using lemon::apply_default_pull_source;

static int passed = 0;
static int failures = 0;

static void check(bool cond, const char* desc) {
    if (cond) {
        std::printf("[PASS] %s\n", desc);
        ++passed;
    } else {
        std::printf("[FAIL] %s\n", desc);
        ++failures;
    }
}

int main() {
    // Absent key falls back to Hugging Face for backward compatibility.
    {
        RuntimeConfig config(json::object());
        check(config.default_model_source() == "huggingface",
              "missing default_model_source falls back to huggingface");
    }

    // Explicit huggingface round-trips.
    {
        RuntimeConfig config(json{{"default_model_source", "huggingface"}});
        check(config.default_model_source() == "huggingface",
              "explicit huggingface preserved");
    }

    // Switching the policy to modelscope is accepted and observable.
    {
        RuntimeConfig config(json{{"default_model_source", "huggingface"}});
        config.set(json{{"default_model_source", "modelscope"}});
        check(config.default_model_source() == "modelscope",
              "default_model_source set to modelscope");
        check(config.snapshot()["default_model_source"] == "modelscope",
              "modelscope reflected in snapshot");
    }

    // Unknown registry names are rejected.
    {
        RuntimeConfig config(json{{"default_model_source", "huggingface"}});
        bool threw = false;
        try {
            config.set(json{{"default_model_source", "nexus"}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "rejects unsupported default_model_source value");
        check(config.default_model_source() == "huggingface",
              "rejected value leaves prior policy intact");
    }

    // Non-string values are rejected.
    {
        RuntimeConfig config(json{{"default_model_source", "huggingface"}});
        bool threw = false;
        try {
            config.set(json{{"default_model_source", 42}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "rejects non-string default_model_source value");
    }

    // ---- /pull source-resolution policy (apply_default_pull_source) ----
    // These mirror the persistence decision the /pull handler makes before a
    // model definition is written, using a non-huggingface default so a bug
    // that hardcodes Hugging Face cannot pass.

    // A source-less registry checkpoint inherits the configured default.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "owner/repo:Q4_K_M"}};
        apply_default_pull_source(req, "modelscope");
        check(req.value("source", "") == "modelscope",
              "source-less registry checkpoint inherits the default");
        check(req["checkpoint"] == "owner/repo:Q4_K_M",
              "registry checkpoint is left untouched");
    }

    // An explicit source is never overridden by the default.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "owner/repo"}, {"source", "huggingface"}};
        apply_default_pull_source(req, "modelscope");
        check(req["source"] == "huggingface",
              "explicit source overrides the configured default");
    }

    // An explicit registry_source alone suppresses default injection.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "owner/repo"}, {"registry_source", "modelscope"}};
        apply_default_pull_source(req, "huggingface");
        check(!req.contains("source"),
              "registry_source suppresses default source injection");
    }

    // A Hugging Face URL is normalized to owner/repo and adopts its registry
    // even when the configured default is ModelScope.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "https://huggingface.co/owner/repo/tree/main"}};
        apply_default_pull_source(req, "modelscope");
        check(req["checkpoint"] == "owner/repo",
              "hugging face URL is normalized to owner/repo");
        check(req.value("source", "") == "huggingface",
              "hugging face URL adopts its own registry over the default");
    }

    // A ModelScope URL is detected regardless of the configured default.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "https://modelscope.cn/models/owner/repo"}};
        apply_default_pull_source(req, "huggingface");
        check(req["checkpoint"] == "owner/repo",
              "modelscope URL is normalized to owner/repo");
        check(req.value("source", "") == "modelscope",
              "modelscope URL adopts its own registry");
    }

    // Self-managed FLM tags are not registry ids: no source is persisted.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "flm"},
                    {"checkpoint", "gemma3:4b"}};
        apply_default_pull_source(req, "modelscope");
        check(!req.contains("source"),
              "self-managed flm checkpoint gets no registry source");
        check(req["checkpoint"] == "gemma3:4b",
              "flm checkpoint is left untouched");
    }

    // Cloud identifiers can look like repo ids but are still self-managed.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "cloud"},
                    {"checkpoint", "openai/gpt-4o"}};
        apply_default_pull_source(req, "modelscope");
        check(!req.contains("source"),
              "self-managed cloud checkpoint gets no registry source");
    }

    // A bare, ownerless checkpoint is not registry-backed.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "just-a-name"}};
        apply_default_pull_source(req, "modelscope");
        check(!req.contains("source"),
              "ownerless checkpoint gets no registry source");
    }

    // Multi-component bodies resolve off checkpoints.main.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "sd-cpp"},
                    {"checkpoints", {{"main", "owner/repo:model.safetensors"}}}};
        apply_default_pull_source(req, "modelscope");
        check(req.value("source", "") == "modelscope",
              "checkpoints.main drives default source resolution");
    }

    // Local imports never receive a remote registry source.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "owner/repo"}, {"local_import", true}};
        apply_default_pull_source(req, "modelscope");
        check(!req.contains("source"),
              "local import gets no remote registry source");
    }

    // A bare refresh with no checkpoint keeps its recorded provenance.
    {
        json req = {{"model_name", "user.M"}};
        apply_default_pull_source(req, "modelscope");
        check(!req.contains("source"),
              "checkpoint-less refresh gets no injected source");
    }

    std::printf("================================================\n");
    if (failures > 0) {
        std::printf("Tests finished: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("All default_model_source tests PASSED (%d passed).\n", passed);
    return 0;
}
