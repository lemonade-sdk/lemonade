#include "lemon/models/locator.h"

namespace lemon {
namespace models {

std::string repo_id_from_checkpoint(const std::string& checkpoint) {
    const size_t colon_pos = checkpoint.find(':');
    return colon_pos != std::string::npos ? checkpoint.substr(0, colon_pos) : checkpoint;
}

std::string variant_from_checkpoint(const std::string& checkpoint) {
    const size_t colon_pos = checkpoint.find(':');
    return colon_pos != std::string::npos ? checkpoint.substr(colon_pos + 1) : std::string();
}

Locator::Locator(LocatorScheme scheme, std::string path, std::string variant,
                 std::optional<RegistryId> registry, std::string revision)
    : scheme(scheme),
      path(std::move(path)),
      variant(std::move(variant)),
      registry(registry),
      revision(std::move(revision)) {}

Locator Locator::from_checkpoint(const std::string& checkpoint, const std::string& source) {
// `source` decides the scheme before the path is read: a Windows absolute path
// contains a colon.
    if (source == "local_path") {
        return Locator(LocatorScheme::Import, checkpoint, std::string(), std::nullopt,
                       std::string());
    }
    if (source == "local_upload") {
        return Locator(LocatorScheme::Cache, checkpoint, std::string(), std::nullopt,
                       std::string());
    }
    if (source == "url") {
        return Locator(LocatorScheme::Url, checkpoint, std::string(), std::nullopt,
                       std::string());
    }

    return Locator(LocatorScheme::Registry, repo_id_from_checkpoint(checkpoint),
                   variant_from_checkpoint(checkpoint), parse_remote_registry_source(source),
                   std::string());
}

std::string Locator::to_string() const {
    switch (scheme) {
        case LocatorScheme::Cache:
            return "local://" + path;
        case LocatorScheme::Import:
            return "import://" + path;
        case LocatorScheme::Url:
            return path.find("://") != std::string::npos ? path : "url://" + path;
        case LocatorScheme::Registry:
            break;
    }

    std::string out = remote_registry_scheme_prefix(*registry) + "://" + path;
    if (!variant.empty()) out += ":" + variant;
    if (!revision.empty()) out += "@" + revision;
    return out;
}

}  // namespace models
}  // namespace lemon
