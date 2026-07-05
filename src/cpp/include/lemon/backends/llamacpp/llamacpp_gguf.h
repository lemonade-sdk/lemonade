#pragma once

#include <string>

namespace lemon {
namespace backends {
namespace llamacpp {

// Resolve the on-disk path of the GGUF file for a model cache directory and
// variant (handles sharding, folder variants, and quant-token fallback). Returns
// the cache directory if no GGUF is present, or "" if the requested variant
// can't be resolved.
std::string resolve_gguf_path(const std::string& model_cache_path, const std::string& variant);

} // namespace llamacpp
} // namespace backends
} // namespace lemon
