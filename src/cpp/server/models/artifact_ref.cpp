#include <lemon/models/artifact_ref.h>

namespace lemon {
namespace models {

bool ArtifactRef::operator==(const ArtifactRef& other) const {
    return registry == other.registry && checkpoint_type == other.checkpoint_type &&
           repo_id == other.repo_id &&
           variant == other.variant && revision == other.revision &&
           required == other.required;
}

}  // namespace models
}  // namespace lemon
