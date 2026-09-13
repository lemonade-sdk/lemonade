#pragma once

#include <string>
#include <vector>

#include "lemon/utils/container_runtime.h"

namespace lemon {
namespace backends {

// One image pin as committed in backend_versions.json.
struct ImagePin {
    std::string recipe;
    std::string variant;  // backend variant, e.g. "rocm" or "vulkan-rocmfpx"
    std::string arch;     // GPU ISA the pin applies to, e.g. "gfx1151"
    utils::ContainerImageRef image;
};

// The pinned image for (recipe, variant) on `arch`. Returns an invalid ref when
// that combination is not published. Image-backed recipes key their
// backend_versions.json entries by arch because the same variant ships from a
// different repository per GPU family.
utils::ContainerImageRef image_pin(const std::string& recipe, const std::string& variant,
                                   const std::string& arch);

// Same, resolved against the host's detected ROCm architecture.
utils::ContainerImageRef image_pin(const std::string& recipe, const std::string& variant);

// True when `recipe`'s descriptor declares it image-backed.
bool recipe_is_image_backed(const std::string& recipe);

// Every committed pin, for the refresh workflow and the drift check.
std::vector<ImagePin> all_image_pins();

// The digest a variant is pinned to on this host, or "" when unpinned. This is
// the "expected version" for the /system-info update-state machinery.
std::string expected_image_digest(const std::string& recipe, const std::string& variant);

// Human-readable registry page for a pin (Docker Hub or GHCR), or "".
std::string registry_url(const utils::ContainerImageRef& ref);

}  // namespace backends
}  // namespace lemon
