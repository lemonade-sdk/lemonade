#include "lemon/models/checkpoint_grammar.h"

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

}  // namespace models
}  // namespace lemon
