#include "lemon/media_model_selection.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using lemon::ModelInfo;
using lemon::ModelType;
using lemon::mcp::MediaModelCandidate;

namespace {

ModelInfo model(ModelType type, std::vector<std::string> labels) {
    ModelInfo info;
    info.type = type;
    info.labels = std::move(labels);
    return info;
}

MediaModelCandidate candidate(const std::string& name,
                              ModelType type,
                              std::vector<std::string> labels) {
    return {name, model(type, std::move(labels))};
}

}  // namespace

int main() {
    const auto normal_image = candidate("normal-image", ModelType::IMAGE, {"image"});
    const auto edit_image = candidate(
        "edit-image", ModelType::IMAGE, {"image", "edit"});
    const auto downloaded_edit = candidate(
        "downloaded-edit", ModelType::IMAGE, {"image", "edit"});

    // #3346 owns the metadata -> capability mapping: edit becomes image-edit.
    const auto capabilities = lemon::ModelManager::model_capabilities(edit_image.info);
    assert(std::find(capabilities.begin(), capabilities.end(), "image") != capabilities.end());
    assert(std::find(capabilities.begin(), capabilities.end(), "image-edit") != capabilities.end());

    // loaded normal image + downloaded edit => downloaded edit
    {
        auto selected = lemon::mcp::select_media_model(
            {normal_image}, {downloaded_edit}, ModelType::IMAGE, "image-edit");
        assert(selected && *selected == "downloaded-edit");
    }

    // loaded edit + downloaded edit => loaded edit
    {
        auto selected = lemon::mcp::select_media_model(
            {edit_image}, {downloaded_edit}, ModelType::IMAGE, "image-edit");
        assert(selected && *selected == "edit-image");
    }

    // loaded normal image only => no eligible model
    {
        auto selected = lemon::mcp::select_media_model(
            {normal_image}, {}, ModelType::IMAGE, "image-edit");
        assert(!selected);
    }

    // explicit-style eligibility: normal image rejected, edit image accepted
    assert(!lemon::mcp::media_model_eligible(
        normal_image.info, ModelType::IMAGE, "image-edit"));
    assert(lemon::mcp::media_model_eligible(
        edit_image.info, ModelType::IMAGE, "image-edit"));

    // An edit label cannot turn a non-image deployment into an image model.
    const auto mislabeled_llm = candidate(
        "mislabeled-llm", ModelType::LLM, {"chat", "edit"});
    assert(!lemon::mcp::media_model_eligible(
        mislabeled_llm.info, ModelType::IMAGE, "image-edit"));

    std::cout << "MCP media model selection tests passed\n";
    return 0;
}
