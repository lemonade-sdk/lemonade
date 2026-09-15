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

// True when this one backend runs from an image rather than a release asset
// (see the nathanw row in llamacpp/llamacpp.h for why that is per backend and
// not per recipe). Arch-independent, so it stays true on a host the variant is
// not published for: it answers "image pin or release tag", which is what
// install and version resolution branch on.
bool backend_is_image_backed(const std::string& recipe, const std::string& backend);

// Every committed pin, for the refresh workflow and the drift check.
std::vector<ImagePin> all_image_pins();

// The digest a variant is pinned to on this host, or "" when unpinned. This is
// the "expected version" for the /system-info update-state machinery, so it is
// shortened the same way resolve_version() shortens the installed one.
std::string expected_image_digest(const std::string& recipe, const std::string& variant);

// "sha256:6d181d74fb6b..." -> "6d181d74fb6b". A full digest is 71 characters and
// overruns every column that shows a backend version next to a release tag like
// "b10723"; twelve hex digits is what the engines themselves display. Pulls and
// installed-digest comparisons always use the full value.
std::string short_digest(const std::string& digest);

// Human-readable registry page for a pin (Docker Hub or GHCR), or "".
std::string registry_url(const utils::ContainerImageRef& ref);

}  // namespace backends
}  // namespace lemon
