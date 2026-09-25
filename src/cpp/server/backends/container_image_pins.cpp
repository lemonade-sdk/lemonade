#include "lemon/backends/container_image_pins.h"

#include <lemon/utils/aixlog.hpp>
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

bool allowed_repository(const std::string& repository) {
    for (const char* prefix : {"docker.io/kyuz0/", "ghcr.io/peonist-ai/"}) {
        if (repository.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

}  // namespace

utils::ContainerImage image_from_entry(const json& node) {
    if (!node.is_object()) return {};
    for (const char* field : {"repository", "tag", "digest", "devices"}) {
        if (!node.contains(field)) {
            LOG(ERROR, "Container") << "Image entry is missing '" << field << "': " << node.dump()
                                    << std::endl;
            return {};
        }
    }
    utils::ContainerImage image;
    image.repository = node.value("repository", "");
    if (!allowed_repository(image.repository)) {
        LOG(ERROR, "Container") << "Image repository " << image.repository
                                << " is not in the allowed list" << std::endl;
        return {};
    }
    image.tag = node.value("tag", "");
    image.digest = node.value("digest", "");
    image.devices = node.value("devices", std::vector<std::string>{});
    const json env = node.value("env", json::object());
    for (const auto& [key, value] : env.items()) {
        image.env.emplace_back(key, value.get<std::string>());
    }
    image.cap_add = node.value("cap_add", std::vector<std::string>{});
    image.ipc_host = node.value("ipc_host", false);
    image.memlock_unlimited = node.value("memlock_unlimited", false);
    return image;
}

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
    return arches.contains(arch) ? image_from_entry(arches[arch]) : utils::ContainerImage{};
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
                if (image.is_object()) pins.push_back({recipe, backend, arch, image_from_entry(image)});
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
