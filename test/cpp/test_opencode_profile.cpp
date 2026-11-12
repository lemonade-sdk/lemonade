#include "lemon_cli/opencode_profile.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

int main() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* name) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    for (const auto& [version, major] : std::vector<std::pair<std::string, int>>{
             {"opencode v2.2.7\n", 2}, {"2.2.7", 2}, {"v2.0.0-beta.1", 2}, {"1.2.3", 1},
             {"opencode v1.9.0", 1}, {"v10.0.0", 10}, {"999.9.9", 999}, {"99999999999999.1.1", 0},
             {"fake-agent-ok", 0}, {"", 0}, {"error: 2.2.7", 0}}) {
        check(lemon_cli::opencode_major_version(version) == major, version.c_str());
    }

    const std::vector<lemon_cli::AgentModelEntry> models = {
        {"Qwen3.8-Flash-Next-Halogen", "Qwen (local)", 65536, {"chat", "tool-calling"}}
    };
    const auto config = lemon_cli::build_opencode_v2_config(
        "https://server.example/v1", "Qwen3.8-Flash-Next-Halogen", models);
    check(config.at("model") == "Lemonade/Qwen3.8-Flash-Next-Halogen", "selected model");
    const auto& provider = config.at("providers").at("Lemonade");
    check(provider.at("package") == "@opencode/ai/providers/openai-compatible", "v2 package");
    check(provider.at("settings").at("baseURL") == "https://server.example/v1", "endpoint");
    check(provider.at("env") == nlohmann::json::array({"LEMONADE_API_KEY"}), "credential environment");
    check(provider.at("models").at("Qwen3.8-Flash-Next-Halogen").at("limit").at("context") == 65536,
          "saved context size");
    check(!provider.contains("npm") && !provider.contains("options"), "v2 schema");

    const auto legacy = lemon_cli::opencode_profile().build_provider_block(
        "http://localhost:8000/v1", "test-key", models);
    check(legacy.at("npm") == "@ai-sdk/openai-compatible", "v1 package preserved");
    check(legacy.at("options").at("apiKey") == "test-key", "v1 credentials preserved");
    return failures ? 1 : 0;
}
