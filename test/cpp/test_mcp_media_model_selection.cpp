#include "lemon/media_model_selection.h"
#include "lemon/model_manager.h"
#include "lemon/mcp_tool.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using lemon::ModelInfo;
using lemon::ModelType;
using lemon::mcp::MediaModelCandidate;

namespace {

int failures = 0;

void check(const char* name, bool condition) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

MediaModelCandidate candidate(std::string name, ModelType type,
                              std::vector<std::string> labels) {
    ModelInfo info;
    info.type = type;
    info.labels = std::move(labels);
    return {std::move(name), type, lemon::ModelManager::model_capabilities(info)};
}

}  // namespace

int main() {
    const auto generation = candidate("generation", ModelType::IMAGE, {"image"});
    const auto editable = candidate("editable", ModelType::IMAGE, {"image", "edit"});
    const auto downloaded = candidate("downloaded-edit", ModelType::IMAGE, {"image", "edit"});
    const auto wrong_type = candidate("wrong-type", ModelType::LLM, {"chat", "edit"});

    const auto& capabilities = editable.capabilities;
    check("server normalizes edit to image-edit",
          std::find(capabilities.begin(), capabilities.end(), "image-edit") !=
              capabilities.end());
    ModelInfo image_info;
    image_info.type = editable.type;
    image_info.labels = {"image", "edit"};
    check("image stays the primary capability",
          lemon::ModelManager::model_capability(image_info) == "image");
    check("generation model is not eligible for edits",
          !lemon::mcp::media_model_eligible(generation, ModelType::IMAGE, "image-edit"));
    check("editable model is eligible",
          lemon::mcp::media_model_eligible(editable, ModelType::IMAGE, "image-edit"));
    check("an edit label does not turn a text model into an image model",
          !lemon::mcp::media_model_eligible(wrong_type, ModelType::IMAGE, "image-edit"));

    auto selected = lemon::mcp::select_media_model(
        {generation}, {downloaded}, ModelType::IMAGE, "image-edit");
    check("downloaded edit model beats loaded generation-only model",
          selected && *selected == "downloaded-edit");

    selected = lemon::mcp::select_media_model(
        {editable}, {downloaded}, ModelType::IMAGE, "image-edit");
    check("loaded eligible edit model wins", selected && *selected == "editable");

    selected = lemon::mcp::select_media_model(
        {generation}, {}, ModelType::IMAGE, "image-edit");
    check("no eligible edit model fails selection", !selected);

    selected = lemon::mcp::select_media_model(
        {wrong_type}, {generation}, ModelType::IMAGE, "image-edit");
    check("no false-positive from a mislabeled text model", !selected);

    // MCP schema validation runs before the handler (and before model loading).
    // The edit tool advertises n <= 10, so the upper bound must be enforced.
    nlohmann::json descriptor = {
        {"name", "test-image-count"},
        {"description", "Validate the MCP image count boundary"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {{"n", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10}}}}},
        }},
    };
    int calls = 0;
    lemon::McpTool count_tool(std::move(descriptor), [&](const nlohmann::json&) {
        ++calls;
        return nlohmann::json::object();
    });
    count_tool.call({{"n", 10}});
    check("MCP image count 10 is accepted", calls == 1);
    bool rejected = false;
    try {
        count_tool.call({{"n", 11}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check("MCP image count 11 is rejected before handler", rejected && calls == 1);

    return failures == 0 ? 0 : 1;
}
