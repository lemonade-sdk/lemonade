#include <lemon/models/artifact_ref.h>

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

bool ArtifactRef::operator==(const ArtifactRef& other) const {
    return registry == other.registry && checkpoint_type == other.checkpoint_type &&
           repo_id == other.repo_id &&
           variant == other.variant && revision == other.revision &&
           required == other.required;
}

}  // namespace models
}  // namespace lemon
