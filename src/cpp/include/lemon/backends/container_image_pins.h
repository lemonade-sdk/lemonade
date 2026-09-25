#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lemon/utils/container_manager.h"

namespace lemon {
namespace backends {

// One image pin as committed in backend_versions.json.
struct ImagePin {
    std::string recipe;
    std::string backend;  // e.g. "rocm" or "nathanw"
    std::string arch;     // GPU ISA the pin applies to, e.g. "gfx1151"
    utils::ContainerImage image;
};

// One per-arch entry as an image. An entry missing a required field, or
// published outside docker.io/kyuz0 and ghcr.io/peonist-ai, is invalid.
utils::ContainerImage image_from_entry(const nlohmann::json& entry);

// The pinned image for (recipe, backend) on `arch`. Returns an invalid image
// when that combination is not published. Container backends key their
// backend_versions.json entries by arch because the same backend ships from a
// different repository per GPU family.
utils::ContainerImage image_pin(const std::string& recipe, const std::string& backend,
                                const std::string& arch);

// Same, resolved against the host's detected ROCm architecture.
utils::ContainerImage image_pin(const std::string& recipe, const std::string& backend);

// True when this backend's backend_versions.json entry is an image instead of a
// release version. Arch-independent, so it stays true on a host the backend is
// not published for: it answers "image or release", which is what install and
// version resolution branch on.
bool backend_is_image_backed(const std::string& recipe, const std::string& backend);

// Every committed pin.
std::vector<ImagePin> all_image_pins();

// The digest a backend is pinned to on this host, or "" when unpinned. This is
// the "expected version" for the /system-info update-state machinery, so it is
// shortened the same way resolve_version() shortens the installed one.
std::string expected_image_digest(const std::string& recipe, const std::string& backend);

// "sha256:6d181d74fb6b..." -> "6d181d74fb6b". A full digest is 71 characters and
// overruns every column that shows a backend version next to a release tag like
// "b10723"; twelve hex digits is what podman and docker themselves display. Pulls and
// installed-digest comparisons always use the full value.
std::string short_digest(const std::string& digest);

// Human-readable registry page for a pin (Docker Hub or GHCR), or "".
std::string registry_url(const utils::ContainerImage& image);

}  // namespace backends
}  // namespace lemon
