#include <lemon/models/artifact_ref.h>

namespace lemon {
namespace models {

ArtifactRef ArtifactRef::from_locator(const Locator& locator, bool required) {
    ArtifactRef ref;
    ref.registry = locator.registry;
    ref.repo_id = locator.path;
    ref.variant = locator.variant;
    ref.revision = locator.revision;
    ref.required = required;
    return ref;
}

bool ArtifactRef::operator==(const ArtifactRef& other) const {
    return registry == other.registry && repo_id == other.repo_id &&
           variant == other.variant && revision == other.revision &&
           required == other.required;
}

}  // namespace models
}  // namespace lemon
