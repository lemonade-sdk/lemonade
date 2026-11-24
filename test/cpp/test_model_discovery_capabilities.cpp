#include "lemon/model_manager.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

}  // namespace

int main() {
    lemon::ModelInfo generation_only;
    generation_only.type = lemon::ModelType::IMAGE;
    generation_only.labels = {"image"};

    check("image model primary capability is image",
          lemon::ModelManager::model_capability(generation_only) == "image");

    const auto generation_capabilities =
        lemon::ModelManager::model_capabilities(generation_only);
    check("generation-only image model exposes image",
          contains(generation_capabilities, "image"));
    check("generation-only image model does not expose image-edit",
          !contains(generation_capabilities, "image-edit"));

    lemon::ModelInfo editable = generation_only;
    editable.labels = {"image", "edit"};

    check("editable image keeps image as primary capability",
          lemon::ModelManager::model_capability(editable) == "image");

    const auto editable_capabilities =
        lemon::ModelManager::model_capabilities(editable);
    check("edit label normalizes to image-edit",
          contains(editable_capabilities, "image-edit"));

    lemon::ModelInfo text_editor;
    text_editor.type = lemon::ModelType::LLM;
    text_editor.labels = {"chat", "edit"};
    check("text model edit label does not advertise image editing",
          !contains(lemon::ModelManager::model_capabilities(text_editor), "image-edit"));

    // Deployment type uses exact lowercase labels. Capability metadata must
    // not promote an uppercase label that deployment type inference ignored.
    lemon::ModelInfo uppercase_image;
    uppercase_image.labels = {"IMAGE"};
    uppercase_image.type = lemon::get_model_type_from_labels(uppercase_image.labels);
    check("uppercase IMAGE does not select image deployment",
          uppercase_image.type == lemon::ModelType::LLM);
    check("uppercase IMAGE does not advertise image on a text model",
          !contains(lemon::ModelManager::model_capabilities(uppercase_image), "image"));

    lemon::ModelInfo uppercase_edit = generation_only;
    uppercase_edit.labels = {"image", "EDIT"};
    check("uppercase EDIT is not a canonical edit capability",
          !contains(lemon::ModelManager::model_capabilities(uppercase_edit), "image-edit"));

    lemon::ModelInfo virtual_router;
    virtual_router.recipe = "collection.router";
    virtual_router.type = lemon::ModelType::LLM;
    check("router collection has router primary capability",
          lemon::ModelManager::model_capability(virtual_router) == "router");

    lemon::ModelInfo virtual_omni;
    virtual_omni.recipe = "collection.omni";
    virtual_omni.type = lemon::ModelType::LLM;
    check("omni collection has omni primary capability",
          lemon::ModelManager::model_capability(virtual_omni) == "omni");

    lemon::ModelInfo video;
    video.type = lemon::ModelType::VIDEO;
    video.labels = {"video"};
    check("video model primary capability is video",
          lemon::ModelManager::model_capability(video) == "video");
    check("video model advertises video capability",
          contains(lemon::ModelManager::model_capabilities(video), "video"));
    check("video model does not advertise image editing",
          !contains(lemon::ModelManager::model_capabilities(video), "image-edit"));

    return failures == 0 ? 0 : 1;
}
