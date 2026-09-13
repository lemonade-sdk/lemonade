#pragma once

#include <string>

namespace lemon {
struct ModelInfo;
}

namespace lemon {
namespace backends {
namespace llamacpp {

// Resolve the on-disk path of the GGUF file for a model cache directory and
// variant (handles sharding, folder variants, and quant-token fallback). Returns
// the cache directory if no GGUF is present, or "" if the requested variant
// can't be resolved.
std::string resolve_gguf_path(const std::string& model_cache_path, const std::string& variant);

// True when a draft checkpoint is a DFlash drafter (named dflash-*.gguf).
bool is_dflash_draft_checkpoint(const std::string& checkpoint);

// Merge the llama-server arguments a model implies (speculative-decode type for
// MTP/DFlash drafters, a pinned parallel slot count) into the user's custom
// args. A user-supplied copy of any of them wins. `merge_args = false` returns
// `custom_args` untouched. Shared by every recipe that serves GGUFs through
// llama-server, containerized or not.
std::string resolve_llamacpp_runtime_args(const ModelInfo& model_info,
                                          const std::string& custom_args, bool merge_args);

// Version of the PATH-installed llama-server, by running it. "" when absent.
std::string system_llamacpp_version();

// True when the ggml HIP plugin the system llama-server needs for ROCm is present.
bool is_ggml_hip_plugin_available();

} // namespace llamacpp
} // namespace backends
} // namespace lemon
