#pragma once

#include <cstdint>
#include <string>
#include "lemon/gguf_capabilities.h"

namespace lemon {
namespace backends {
namespace llamacpp {

// GGUF metadata extracted in a single pass over the KV header. This is
// llama.cpp-specific model introspection; it lives in the llamacpp backend
// folder rather than in the shared model manager.
struct GgufMetadata {
    std::string architecture;
    int64_t context_length = 0;
    int64_t block_count = 0;
    int64_t embedding_length = 0;
    int64_t head_count_kv = 0;
    int64_t key_length = 0;
    GgufCapabilities caps;
};

// Read GGUF metadata from a .gguf file. Returns false if the file is missing or
// not a valid GGUF container.
bool read_gguf_metadata(GgufMetadata& out, const std::string& path);

} // namespace llamacpp
} // namespace backends
} // namespace lemon
