#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {

// Single GGUF variant detected in a Hugging Face repository.
struct GgufVariant {
    std::string name;          // Quant token (e.g. "Q4_K_M") or folder/file name fallback.
    std::string primary_file;  // First file (lexicographic) representing this variant.
    std::vector<std::string> files;  // All files belonging to this variant.
    bool sharded = false;
    uint64_t size_bytes = 0;   // Sum of file sizes (0 if unknown).
};

// Result of enumerating GGUF variants in a HF repo file listing.
struct GgufVariantSet {
    std::vector<GgufVariant> variants;
    std::vector<std::string> mmproj_files;  // Bare filenames of mmproj-*.gguf files.
};

// Enumerate GGUF variants from a list of files in a HuggingFace repository.
//
// `repo_files` should be the rfilenames from the HF /api/models/<id> "siblings"
// array. `file_sizes` is an optional map (rfilename -> size in bytes) used to
// populate per-variant size totals; missing entries are treated as 0.
//
// Mirrors the JS logic in src/app/src/renderer/ModelManager.tsx detectBackend()
// for the GGUF branch.
GgufVariantSet enumerate_gguf_variants(
    const std::vector<std::string>& repo_files,
    const std::vector<std::pair<std::string, uint64_t>>& file_sizes = {},
    size_t max_variants = 0);

// Build the JSON response body for GET /api/v{0,1}/pull/variants.
//
// Performs the HTTP call to HuggingFace, runs `enumerate_gguf_variants`,
// derives suggested labels from the repo id, and returns the response JSON.
// Throws std::runtime_error on transport failure; sets `not_found` true if
// the HF API returned 404 so the caller can return an HTTP 404.
nlohmann::json fetch_pull_variants(const std::string& checkpoint, bool& not_found);

}  // namespace lemon
