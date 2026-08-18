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

    // When both fields are present, `checkpoints` is authoritative and its
    // `main` entry drives resolution — matching the ModelManager precedence.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "flm"},
                    {"checkpoint", "owner/repo"},
                    {"checkpoints", {{"main", "gemma3:4b"}}}};
        apply_default_pull_source(req, "modelscope");
        check(!req.contains("source"),
              "checkpoints.main takes precedence over a stray top-level checkpoint");
        check(req["checkpoint"] == "owner/repo",
              "the ignored top-level checkpoint is left untouched");
    }

    // A provider URL that contradicts an explicit source is a 400 (throws).
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "https://huggingface.co/owner/repo"},
                    {"source", "modelscope"}};
        bool threw = false;
        try {
            apply_default_pull_source(req, "huggingface");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "provider URL conflicting with explicit source throws");
    }

    // registry_source is honored the same way for conflict detection.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "https://modelscope.cn/models/owner/repo"},
                    {"registry_source", "huggingface"}};
        bool threw = false;
        try {
            apply_default_pull_source(req, "huggingface");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "provider URL conflicting with registry_source throws");
    }

    // A provider URL that agrees with the explicit source is accepted and
    // normalized without re-injecting the source.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "https://modelscope.cn/models/owner/repo"},
                    {"source", "modelscope"}};
        apply_default_pull_source(req, "huggingface");
        check(req["checkpoint"] == "owner/repo",
              "agreeing provider URL is still normalized to owner/repo");
        check(req["source"] == "modelscope",
              "agreeing explicit source is preserved");
    }

    // Secondary checkpoints carrying provider URLs are normalized too, not just
    // main, and the whole model adopts the shared registry.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "sd-cpp"},
                    {"checkpoints",
                     {{"main", "https://modelscope.cn/models/owner/repo"},
                      {"vae", "https://modelscope.cn/models/owner/vae"}}}};
        apply_default_pull_source(req, "huggingface");
        check(req["checkpoints"]["main"] == "owner/repo",
              "main checkpoint URL is normalized");
        check(req["checkpoints"]["vae"] == "owner/vae",
              "secondary checkpoint URL is normalized");
        check(req.value("source", "") == "modelscope",
              "multi-checkpoint model adopts the URL registry");
    }

    // Checkpoints disagreeing on the registry are rejected.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "sd-cpp"},
                    {"checkpoints",
                     {{"main", "https://huggingface.co/owner/repo"},
                      {"vae", "https://modelscope.cn/models/owner/vae"}}}};
        bool threw = false;
        try {
            apply_default_pull_source(req, "huggingface");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "checkpoints from different registries are rejected");
    }

    // A non-string `source` must be rejected before URL normalization, so a
    // provider URL cannot silently overwrite it with the URL's registry.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "https://huggingface.co/owner/repo"},
                    {"source", 123}};
        bool threw = false;
        try {
            apply_default_pull_source(req, "huggingface");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "non-string source with a provider URL is rejected");
    }

    // The same guard applies to `registry_source`.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "owner/repo"},
                    {"registry_source", true}};
        bool threw = false;
        try {
            apply_default_pull_source(req, "huggingface");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        check(threw, "non-string registry_source is rejected");
    }

    // A JSON null is treated as absent, not as an invalid value.
    {
        json req = {{"model_name", "user.M"}, {"recipe", "llamacpp"},
                    {"checkpoint", "owner/repo"},
                    {"source", nullptr}};
        apply_default_pull_source(req, "modelscope");
        check(req.value("source", "") == "modelscope",
              "null source falls back to the configured default");
    }

    std::printf("================================================\n");
    if (failures > 0) {
        std::printf("Tests finished: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("All default_model_source tests PASSED (%d passed).\n", passed);
    return 0;
}
