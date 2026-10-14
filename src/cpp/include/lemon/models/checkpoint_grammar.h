#pragma once

#include <string>

namespace lemon {
namespace models {

// The `repo_id[:variant]` grammar, split at the first colon.
std::string repo_id_from_checkpoint(const std::string& checkpoint);
std::string variant_from_checkpoint(const std::string& checkpoint);

}  // namespace models
}  // namespace lemon
