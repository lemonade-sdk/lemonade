#include "lemon_cli/tui_launch_model.h"
#include "lemon_cli/tui_launch_worker.h"

#include <cstdio>
#include <future>
#include <stdexcept>

using nlohmann::json;
using namespace lemon_cli;

namespace {
int failures = 0;
void check(const char* name, bool success) {
    std::printf("%s %s\n", success ? "PASS" : "FAIL", name);
    if (!success) ++failures;
}
}

int main() {
    check("missing choices open TUI", launch_uses_tui(true, std::nullopt, "", ""));
    check("missing model opens TUI", launch_uses_tui(true, std::nullopt, "pi", ""));
    check("complete command stays direct", !launch_uses_tui(true, std::nullopt, "pi", "model"));
    check("explicit TUI overrides complete command", launch_uses_tui(true, true, "pi", "model"));
    check("explicit text mode stays text", !launch_uses_tui(true, false, "", ""));
    check("pipes never open TUI", !launch_uses_tui(false, true, "", ""));

    const auto models = parse_launch_models({{"data", json::array({
        {{"id", "Qwen3.5-test"}, {"recipe", "llamacpp"}, {"checkpoint", "repo:current.gguf"}, {"downloaded", true},
         {"labels", {"chat", "tool-calling", "hot"}}, {"size", 4.5},
         {"recipe_options", {{"ctx_size", 8192}}}},
        {{"id", "small"}, {"recipe", "flm"}, {"downloaded", false},
         {"labels", {"chat", "tool-calling", "hot"}}, {"size", -1}},
        {{"id", "plain"}, {"recipe", "llamacpp"}, {"downloaded", true}, {"labels", {"chat"}}},
        {{"id", "embed"}, {"recipe", "llamacpp"}, {"downloaded", true}, {"labels", {"embedding"}}},
        {{"id", "incomplete"}, {"recipe", "llamacpp"}, {"downloaded", true}},
        nullptr
    })}}, {{"all_models_loaded", json::array({{{"model_name", "Qwen3.5-test"}, {"recipe", "llamacpp"},
        {"checkpoint", "repo:current.gguf"}, {"status", "ready"}}})}});
    check("parse storage independently of runtime", models[0].downloaded && models[0].runtime == "ready" && models[2].runtime == "unloaded");
    check("saved context retained for preview", models[0].recipe_options["ctx_size"] == 8192);
    const auto stale = parse_launch_models({{"data", json::array({{{"id", "replaced"}, {"recipe", "llamacpp"},
        {"checkpoint", "repo:new.gguf"}}})}}, {{"all_models_loaded", json::array({{{"model_name", "replaced"},
        {"recipe", "llamacpp"}, {"checkpoint", "repo:old.gguf"}, {"status", "ready"}}})}});
    check("stale loaded checkpoint cannot claim readiness", stale.front().runtime != "ready");
    const auto metadata_listing = json{{"data", json::array({{{"id", "metadata"}, {"recipe", "llamacpp"},
        {"checkpoint", "repo:current.gguf"}}})}};
    for (const auto& entry : {json{{"model_name", "metadata"}, {"recipe", "llamacpp"}, {"checkpoint", 42}},
                             json{{"model_name", "metadata"}, {"recipe", "llamacpp"}, {"checkpoint", "repo:current.gguf"}}}) {
        bool unknown = false;
        try { unknown = parse_launch_models(metadata_listing, {{"all_models_loaded", json::array({entry})}}).front().runtime == "unknown"; }
        catch (const std::exception&) {}
        check("incomplete health metadata preserves catalog with unknown runtime", unknown);
    }
    check("unknown size is not zero bytes", models[0].size_gb == 4.5 && !models[1].size_gb);
    check("downloaded filter excludes other modalities", filter_launch_models(models, LaunchTab::Downloaded, "", "pi") == std::vector<int>({0, 4, 2}));
    check("search includes labels case insensitively", filter_launch_models(models, LaunchTab::All, "TOOL-CALLING", "pi") == std::vector<int>({0, 1}));
    check("recommended avoids Codex family warning", filter_launch_models(models, LaunchTab::Recommended, "", "codex") == std::vector<int>({1}));
    check("recommended requires tool calling", filter_launch_models(models, LaunchTab::Recommended, "", "pi") == std::vector<int>({0, 1}));
    check("all keeps warned choices available", filter_launch_models(models, LaunchTab::All, "qwen", "codex") == std::vector<int>({0}));
    check("missing tool metadata has warning", !launch_model_warning(models[2], "pi").empty());
    check("preset tab does not use model indices", filter_launch_models(models, LaunchTab::Presets, "", "pi").empty());
    bool rejected = false;
    try { parse_launch_models(json::object(), json::object()); }
    catch (const std::runtime_error&) { rejected = true; }
    check("malformed listing is not an empty catalog", rejected);

    const json preset = {{"model_name", "legacy-preset"}, {"recipe", "llamacpp"},
                         {"checkpoints", {{"main", "repo:weights.gguf"}}}, {"source", "huggingface"},
                         {"labels", {"vision", "tool-calling", "hot"}},
                         {"recipe_options", {{"ctx_size", 0}}}, {"size", 19.7}};
    const auto preview = parse_launch_preset(preset);
    check("legacy presets infer chat without a chat label", preview.id == "user.legacy-preset" && preview.recipe_options["ctx_size"] == 0);
    check("preset parsing never changes the imported payload", preset["model_name"] == "legacy-preset" && !preset.contains("id"));
    auto claimed_download = preset;
    claimed_download["downloaded"] = true;
    check("remote preset cannot claim server storage", !parse_launch_preset(claimed_download).storage_known);
    auto existing = preview;
    existing.id = "legacy-preset";
    existing.downloaded = true;
    existing.storage_known = true;
    existing.runtime = "ready";
    const auto cached = reconcile_launch_preset(preview, {existing});
    check("matching server checkpoints establish storage", cached.downloaded && cached.storage_known && cached.runtime == "ready");
    auto other_registry = preset;
    other_registry["source"] = "modelscope";
    check("other registry cannot claim the same checkpoint cache", !reconcile_launch_preset(parse_launch_preset(other_registry), {existing}).storage_known);
    existing.checkpoints["main"] = "repo:different.gguf";
    check("a matching name with different weights stays unknown", !reconcile_launch_preset(preview, {existing}).storage_known);
    rejected = false;
    try { parse_launch_preset({{"model_name", "embedding-preset"}, {"recipe", "llamacpp"},
                              {"checkpoint", "repo:weights.gguf"}, {"labels", {"embedding"}}}); }
    catch (const std::runtime_error&) { rejected = true; }
    check("non-chat presets rejected before import", rejected);
    for (const auto& invalid : {json{{"model_name", "missing-weights"}, {"recipe", "llamacpp"}},
                               json{{"model_name", "bad-options"}, {"recipe", "llamacpp"},
                                    {"checkpoint", "repo:weights.gguf"}, {"recipe_options", 7}},
                               json{{"model_name", "user."}, {"recipe", "llamacpp"},
                                    {"checkpoint", "repo:weights.gguf"}}}) {
        rejected = false;
        try { parse_launch_preset(invalid); }
        catch (const std::runtime_error&) { rejected = true; }
        check("malformed preset cannot be launched", rejected);
    }
    check("preset URL keeps nested directories as path segments",
          launch_preset_url("coding-agents/strix halo", "qwen coder.json") ==
          "https://raw.githubusercontent.com/lemonade-sdk/recipes/main/coding-agents/strix%20halo/qwen%20coder.json");

    json options = {{"ctx_size", 4096}, {"llamacpp_args", "--keep-this"}};
    std::string error;
    check("inherit removes only context override", apply_launch_context("inherit", options, error) && !options.contains("ctx_size") && options["llamacpp_args"] == "--keep-this");
    check("auto is explicit", apply_launch_context("auto", options, error) && options["ctx_size"] == -1);
    check("custom tokens accepted", apply_launch_context("8192", options, error) && options["ctx_size"] == 8192);
    for (const auto* invalid : {"0", "-2", "8junk", "1.5", "99999999999999999999999999", "2147483648"}) {
        const auto before = options;
        check(invalid, !apply_launch_context(invalid, options, error) && options == before && !error.empty());
    }
    LaunchWorker worker;
    std::promise<void> completed;
    bool applied = false;
    worker.submit([&completed] { completed.set_value(); }, [&applied] { applied = true; });
    completed.get_future().wait();
    worker.wait_for_results(std::chrono::milliseconds(1000));
    check("early worker completions survive until UI drain", !applied && worker.drain() && applied);
    worker.stop();

    auto gate = std::make_shared<std::promise<void>>();
    auto started = std::make_shared<std::promise<void>>();
    auto ended = std::make_shared<std::promise<void>>();
    auto released = gate->get_future().share();
    auto stopped = std::make_unique<LaunchWorker>();
    stopped->submit([started, ended, released] {
        started->set_value();
        released.wait();
        ended->set_value();
    }, [] {});
    started->get_future().wait();
    const auto before = std::chrono::steady_clock::now();
    stopped.reset();
    check("worker shutdown never waits for network work", std::chrono::steady_clock::now() - before < std::chrono::milliseconds(500));
    gate->set_value();
    ended->get_future().wait();
    return failures == 0 ? 0 : 1;
}
