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

utils::ContainerImage parse_image(const json& node) {
    utils::ContainerImage image;
    if (!node.is_object()) return image;
    image.repository = node.value("repository", "");
    image.tag = node.value("tag", "");
    image.digest = node.value("digest", "");
    image.channel = node.value("channel", "stable");
    image.devices = node.value("devices", std::vector<std::string>{});
    for (const auto& [key, value] : node.value("env", json::object()).items()) {
        image.env.emplace_back(key, value.get<std::string>());
    }
    image.cap_add = node.value("cap_add", std::vector<std::string>{});
    image.ipc_host = node.value("ipc_host", false);
    image.memlock_unlimited = node.value("memlock_unlimited", false);
    return image;
}

}  // namespace

bool backend_is_image_backed(const std::string& recipe, const std::string& backend) {
    const json& data = versions();
    if (!data.contains(recipe) || !data[recipe].contains(backend)) return false;
    const json& arches = data[recipe][backend];
    if (!arches.is_object()) return false;
    for (const auto& [arch, image] : arches.items()) {
        if (image.is_object() && image.contains("repository")) return true;
    }
    return false;
}

utils::ContainerImage image_pin(const std::string& recipe, const std::string& backend,
                                const std::string& arch) {
    if (arch.empty() || !backend_is_image_backed(recipe, backend)) return {};
    const json& arches = versions()[recipe][backend];
    return arches.contains(arch) ? parse_image(arches[arch]) : utils::ContainerImage{};
}

utils::ContainerImage image_pin(const std::string& recipe, const std::string& backend) {
    return image_pin(recipe, backend, SystemInfo::get_rocm_arch());
}

std::string short_digest(const std::string& digest) {
    const std::string kPrefix = "sha256:";
    const std::string hex =
        digest.rfind(kPrefix, 0) == 0 ? digest.substr(kPrefix.size()) : digest;
    return hex.size() > 12 ? hex.substr(0, 12) : hex;
}

std::string expected_image_digest(const std::string& recipe, const std::string& backend) {
    return short_digest(image_pin(recipe, backend).digest);
}

std::vector<ImagePin> all_image_pins() {
    std::vector<ImagePin> pins;
    const json& data = versions();
    for (const auto* descriptor : all_descriptors()) {
        const std::string& recipe = descriptor->recipe;
        if (!data.contains(recipe) || !data[recipe].is_object()) continue;
        for (const auto& [backend, arches] : data[recipe].items()) {
            if (!backend_is_image_backed(recipe, backend)) continue;
            for (const auto& [arch, image] : arches.items()) {
                if (image.is_object()) pins.push_back({recipe, backend, arch, parse_image(image)});
            }
        }
    }
    return pins;
}

std::string registry_url(const utils::ContainerImage& image) {
    if (!image.valid()) return "";
    const std::string& repository = image.repository;
    const std::string kDockerIo = "docker.io/";
    const std::string kGhcr = "ghcr.io/";
    if (repository.rfind(kDockerIo, 0) == 0) {
        return "https://hub.docker.com/r/" + repository.substr(kDockerIo.size()) + "/tags";
    }
    if (repository.rfind(kGhcr, 0) == 0) {
        return "https://github.com/" + repository.substr(kGhcr.size()) + "/pkgs/container/" +
               repository.substr(repository.find_last_of('/') + 1);
    }
    return "";
}

}  // namespace backends
}  // namespace lemon
