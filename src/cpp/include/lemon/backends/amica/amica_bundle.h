#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace lemon {
namespace backends {
namespace amica {

// The fixed context length of an AMICA ChatApp bundle, read from its decoder
// graph as amica-engine reads it: the attention-mask length for token-input
// bundles, else the KV cache's sequence dimension. nullopt if unreadable.
std::optional<int64_t> bundle_context_length(const std::string& model_dir);

}  // namespace amica
}  // namespace backends
}  // namespace lemon
