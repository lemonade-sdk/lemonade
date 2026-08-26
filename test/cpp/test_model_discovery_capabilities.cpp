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

    return failures == 0 ? 0 : 1;
}
