#include "lemon/backends/container_image_pins.h"

#include <nlohmann/json.hpp>

#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/system_info.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

namespace lemon {
namespace backends {

using json = nlohmann::json;

namespace {

const json& versions() {
    static const json data = []() -> json {
        try {
            return utils::JsonUtils::load_from_file(
                utils::get_resource_path("resources/backend_versions.json"));
        } catch (...) {
            return json::object();
        }
    }();
    return data;
}

utils::ContainerImageRef parse_ref(const json& node) {
    utils::ContainerImageRef ref;
    if (!node.is_object()) return ref;
    ref.repository = node.value("repository", "");
    ref.tag = node.value("tag", "");
    ref.digest = node.value("digest", "");
    ref.channel = node.value("channel", "stable");
    return ref;
}

}  // namespace

bool recipe_is_image_backed(const std::string& recipe) {
    const auto* descriptor = descriptor_for(recipe);
    return descriptor && descriptor->image_backed;
}

utils::ContainerImageRef image_pin(const std::string& recipe, const std::string& variant,
                                   const std::string& arch) {
    const json& data = versions();
    if (!data.contains(recipe) || !data[recipe].is_object()) return {};
    const json& recipe_node = data[recipe];
    if (!recipe_node.contains(variant) || !recipe_node[variant].is_object()) return {};

    const json& variant_node = recipe_node[variant];
    if (variant_node.contains("repository")) {
        return parse_ref(variant_node);  // arch-agnostic pin
    }
    if (arch.empty() || !variant_node.contains(arch)) return {};
    return parse_ref(variant_node[arch]);
}

utils::ContainerImageRef image_pin(const std::string& recipe, const std::string& variant) {
    return image_pin(recipe, variant, SystemInfo::get_rocm_arch());
}

std::string short_digest(const std::string& digest) {
    const std::string kPrefix = "sha256:";
    const std::string hex =
        digest.rfind(kPrefix, 0) == 0 ? digest.substr(kPrefix.size()) : digest;
    return hex.size() > 12 ? hex.substr(0, 12) : hex;
}

std::string expected_image_digest(const std::string& recipe, const std::string& variant) {
    return short_digest(image_pin(recipe, variant).digest);
}

std::vector<ImagePin> all_image_pins() {
    std::vector<ImagePin> pins;
    const json& data = versions();
    for (const auto* descriptor : all_descriptors()) {
        if (!descriptor->image_backed) continue;
        const std::string& recipe = descriptor->recipe;
        if (!data.contains(recipe) || !data[recipe].is_object()) continue;
        for (auto variant_it = data[recipe].begin(); variant_it != data[recipe].end();
             ++variant_it) {
            if (!variant_it.value().is_object()) continue;
            if (variant_it.value().contains("repository")) {
                pins.push_back({recipe, variant_it.key(), "", parse_ref(variant_it.value())});
                continue;
            }
            for (auto arch_it = variant_it.value().begin(); arch_it != variant_it.value().end();
                 ++arch_it) {
                if (!arch_it.value().is_object()) continue;
                pins.push_back(
                    {recipe, variant_it.key(), arch_it.key(), parse_ref(arch_it.value())});
            }
        }
    }
    return pins;
}

std::string registry_url(const utils::ContainerImageRef& ref) {
    if (!ref.valid()) return "";
    const std::string kDockerIo = "docker.io/";
    const std::string kGhcr = "ghcr.io/";
    if (ref.repository.rfind(kDockerIo, 0) == 0) {
        return "https://hub.docker.com/r/" + ref.repository.substr(kDockerIo.size()) + "/tags";
    }
    if (ref.repository.rfind(kGhcr, 0) == 0) {
        return "https://github.com/" + ref.repository.substr(kGhcr.size()) + "/pkgs/container/" +
               ref.repository.substr(ref.repository.find_last_of('/') + 1);
    }
    return "";
}

}  // namespace backends
}  // namespace lemon
