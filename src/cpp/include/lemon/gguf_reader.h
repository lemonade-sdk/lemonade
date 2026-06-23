#pragma once

/// GGUF binary format reader and metadata extraction.
///
/// Provides a single-pass reader over the GGUF KV-header that extracts
/// architecture name, context length, block count, embedding dimension,
/// attention head / key dimensions, and capability hints (vision, tool-calling,
/// MTP).  All helpers are static — no stateful reader class is needed because
/// the header is read sequentially in one shot.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <vector>
#include <lemon/gguf_capabilities.h>
#include <lemon/utils/path_utils.h>

namespace lemon {

/// Metadata extracted from a GGUF file's KV header.
///
/// Stored inside ModelInfo for llama.cpp models so that auto-tune and other
/// subsystems can estimate KV-cache pressure and clamp context size.
///
/// Arrays that vary per-block (head_count_kv_per_layer, sliding_window_pattern)
/// are stored as-is so that downstream code can perform precise per-layer
/// computations (e.g. distinguishing SWA-layer head counts from full-layer
/// head counts).  Scalar convenience fields (head_count_kv, swa_layer_count)
/// are derived from the raw arrays after the header pass completes.
struct GgufMetadata {
    std::string architecture;
    int64_t context_length = 0;
    int64_t block_count = 0;
    int64_t embedding_length = 0;
    int64_t head_count_kv = 0;       // total across all blocks (derived)
    int64_t key_length = 0;
    int64_t key_length_swa = 0;      // SWA-reduced key length (Gemma4, etc.)
    int64_t swa_layer_count = 0;     // layers with sliding-window attention (derived)
    int64_t full_attention_interval = 0; // every Nth layer does full attention (Qwen SSM)
    GgufCapabilities caps;

    // ── Raw per-layer arrays (stored as read from GGUF) ───────────────
    // Populated when the GGUF field is an array.  Empty when the field is
    // a scalar (in which case the scalar is repeated for every block).
    std::vector<int64_t> head_count_kv_per_layer;
    std::vector<bool> sliding_window_pattern;

    // Transient scalar held when head_count_kv GGUF field is a scalar.
    // Converted to head_count_kv_per_layer in post-loop validation.
    int64_t head_count_kv_scalar = 0;
};

// ── Low-level binary readers ──────────────────────────────────────────

template <typename T>
static bool read_gguf_le(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

static bool read_gguf_string(std::istream& in, std::string& value) {
    uint64_t len = 0;
    if (!read_gguf_le(in, len)) return false;
    if (len > 1024 * 1024) return false;
    value.assign(static_cast<size_t>(len), '\0');
    if (len == 0) return true;
    in.read(&value[0], static_cast<std::streamsize>(len));
    return static_cast<bool>(in);
}

static bool skip_gguf_bytes(std::istream& in, uint64_t bytes) {
    if (bytes > INT64_MAX) return false;
    in.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
    return static_cast<bool>(in);
}

static uint64_t gguf_scalar_size(uint32_t type) {
    switch (type) {
        case 0:  // UINT8
        case 1:  // INT8
        case 7:  // BOOL
            return 1;
        case 2:  // UINT16
        case 3:  // INT16
            return 2;
        case 4:  // UINT32
        case 5:  // INT32
        case 6:  // FLOAT32
            return 4;
        case 10: // UINT64
        case 11: // INT64
        case 12: // FLOAT64
            return 8;
        default:
            return 0;
    }
}

// Forward declaration (mutual recursion with integer reader)
static bool skip_gguf_value(std::istream& in, uint32_t type);

static bool read_gguf_integer_value(std::istream& in, uint32_t type, int64_t& value) {
    switch (type) {
        case 0: { uint8_t v = 0; if (!read_gguf_le(in, v)) return false; value = v; return true; }
        case 1: { int8_t v = 0; if (!read_gguf_le(in, v)) return false; value = v; return true; }
        case 2: { uint16_t v = 0; if (!read_gguf_le(in, v)) return false; value = v; return true; }
        case 3: { int16_t v = 0; if (!read_gguf_le(in, v)) return false; value = v; return true; }
        case 4: { uint32_t v = 0; if (!read_gguf_le(in, v)) return false; value = v; return true; }
        case 5: { int32_t v = 0; if (!read_gguf_le(in, v)) return false; value = v; return true; }
        case 10: {
            uint64_t v = 0;
            if (!read_gguf_le(in, v)) return false;
            if (v > INT64_MAX) return false;
            value = static_cast<int64_t>(v);
            return true;
        }
        case 11: { int64_t v = 0; if (!read_gguf_le(in, v)) return false; value = v; return true; }
        default:
            return skip_gguf_value(in, type) && false;
    }
}

static bool skip_gguf_value(std::istream& in, uint32_t type) {
    if (type == 8) {  // STRING
        std::string ignored;
        return read_gguf_string(in, ignored);
    }

    if (type == 9) {  // ARRAY
        uint32_t elem_type = 0;
        uint64_t count = 0;
        if (!read_gguf_le(in, elem_type) || !read_gguf_le(in, count)) return false;

        if (elem_type == 8) {
            for (uint64_t i = 0; i < count; ++i) {
                std::string ignored;
                if (!read_gguf_string(in, ignored)) return false;
            }
            return true;
        }

        if (elem_type == 9) return false;
        uint64_t elem_size = gguf_scalar_size(elem_type);
        if (elem_size == 0) return false;
        if (count > INT64_MAX / elem_size) return false;
        return skip_gguf_bytes(in, count * elem_size);
    }

    uint64_t size = gguf_scalar_size(type);
    return size > 0 && skip_gguf_bytes(in, size);
}

// ── Helpers used by the metadata reader ───────────────────────────────

namespace gguf_reader_detail {

inline std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

inline bool ends_with_ignore_case(const std::string& str, const std::string& suffix) {
    if (suffix.length() > str.length()) return false;
    return to_lower(str.substr(str.length() - suffix.length())) == to_lower(suffix);
}

inline bool starts_with_ignore_case(const std::string& str, const std::string& prefix) {
    if (prefix.length() > str.length()) return false;
    return to_lower(str.substr(0, prefix.length())) == to_lower(prefix);
}

inline bool contains_ignore_case(const std::string& str, const std::string& substr) {
    return to_lower(str).find(to_lower(substr)) != std::string::npos;
}

inline bool has_gguf_magic(const std::string& path) {
    std::ifstream in(utils::path_from_utf8(path), std::ios::binary);
    if (!in.is_open()) return false;
    char magic[4] = {};
    in.read(magic, sizeof(magic));
    return in.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
           std::memcmp(magic, "GGUF", 4) == 0;
}

} // namespace gguf_reader_detail

// ── Public API ────────────────────────────────────────────────────────

/// Read GGUF metadata from a file in a single pass over the KV header.
///
/// Returns true on success (out is populated).  Returns false if the file
/// cannot be opened or does not have a valid GGUF header.
inline bool read_gguf_metadata(GgufMetadata& out, const std::string& path) {
    std::ifstream in(lemon::utils::path_from_utf8(path), std::ios::binary);
    if (!in) return false;

    char magic[4] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, "GGUF", 4) != 0) return false;

    uint32_t version = 0;
    uint64_t tensor_count = 0;
    uint64_t kv_count = 0;
    if (!read_gguf_le(in, version) || !read_gguf_le(in, tensor_count) || !read_gguf_le(in, kv_count)) {
        return false;
    }
    (void)version;
    (void)tensor_count;

    int64_t pending_context_length = 0;

    for (uint64_t i = 0; i < kv_count; ++i) {
        std::string key;
        uint32_t type = 0;
        if (!read_gguf_string(in, key) || !read_gguf_le(in, type)) return false;

        // Read architecture
        if (key == "general.architecture" && type == 8) {
            if (!read_gguf_string(in, out.architecture)) return false;
            if (pending_context_length > 0) {
                out.context_length = pending_context_length;
            }
            continue;
        }

        // Context length
        const bool context_key = !out.architecture.empty()
                                 && key == out.architecture + ".context_length";
        const bool possible_context_key =
            out.architecture.empty() && key.size() > std::strlen(".context_length")
            && gguf_reader_detail::ends_with_ignore_case(key, ".context_length");
        if (context_key || possible_context_key) {
            int64_t value = 0;
            if (read_gguf_integer_value(in, type, value) && value > 0) {
                if (context_key) {
                    out.context_length = value;
                } else {
                    pending_context_length = value;
                }
            }
            continue;
        }

        // Architecture fields for KV cache estimation
        if (!out.architecture.empty()) {
            if (key == out.architecture + ".block_count") {
                int64_t value = 0;
                if (read_gguf_integer_value(in, type, value) && value > 0)
                    out.block_count = value;
                continue;
            }
            if (key == out.architecture + ".embedding_length") {
                int64_t value = 0;
                if (read_gguf_integer_value(in, type, value) && value > 0)
                    out.embedding_length = value;
                continue;
            }
            if (key == out.architecture + ".attention.head_count_kv") {
                if (type == 9) {
                    // Array of integers: one entry per block — store raw values
                    uint32_t elem_type = 0;
                    uint64_t count = 0;
                    if (read_gguf_le(in, elem_type) && read_gguf_le(in, count)) {
                        out.head_count_kv_per_layer.resize(static_cast<size_t>(count));
                        for (uint64_t j = 0; j < count; ++j) {
                            int64_t v = 0;
                            if (read_gguf_integer_value(in, elem_type, v))
                                out.head_count_kv_per_layer[j] = v;
                        }
                    }
                } else {
                    // Scalar: per-block count, stored transiently for later
                    int64_t value = 0;
                    if (read_gguf_integer_value(in, type, value) && value > 0)
                        out.head_count_kv_scalar = value;
                }
                continue;
            }
            if (key == out.architecture + ".attention.key_length") {
                int64_t value = 0;
                if (read_gguf_integer_value(in, type, value) && value > 0)
                    out.key_length = value;
                continue;
            }
            if (key == out.architecture + ".attention.key_length_swa") {
                int64_t value = 0;
                if (read_gguf_integer_value(in, type, value) && value > 0)
                    out.key_length_swa = value;
                continue;
            }
            if (key == out.architecture + ".full_attention_interval") {
                int64_t value = 0;
                if (read_gguf_integer_value(in, type, value) && value > 0)
                    out.full_attention_interval = value;
                continue;
            }
        }

        // sliding_window_pattern: bool array, one entry per layer.
        // Store raw values so downstream code can correlate with per-layer
        // head counts and key lengths.  Match by suffix so we don't need the
        // architecture prefix (key shape: arch.attention.sliding_window_pattern).
        if (key.size() > std::strlen(".sliding_window_pattern")
             && gguf_reader_detail::ends_with_ignore_case(key, ".sliding_window_pattern") && type == 9) {
            uint32_t elem_type = 0;
            uint64_t count = 0;
            if (read_gguf_le(in, elem_type) && read_gguf_le(in, count)) {
                if (elem_type == 7) {  // BOOL
                    out.sliding_window_pattern.resize(static_cast<size_t>(count));
                    for (uint64_t j = 0; j < count; ++j) {
                        uint8_t v = 0;
                        if (read_gguf_le(in, v))
                            out.sliding_window_pattern[j] = (v != 0);
                    }
                } else if (elem_type != 9) {
                    uint64_t elem_size = gguf_scalar_size(elem_type);
                    if (elem_size > 0)
                        skip_gguf_bytes(in, count * elem_size);
                }
            }
            continue;
        }

        // Capability detection (vision, tool-calling, MTP)
        if (type == 4) {
            uint32_t val = 0;
            if (read_gguf_le(in, val)) {
                if (gguf_reader_detail::contains_ignore_case(key, "nextn_predict_layers") && val > 0)
                    out.caps.mtp = true;
            }
        } else if (type == 8) {
            std::string value;
            if (read_gguf_string(in, value)) {
                inspect_gguf_string(key, value, out.caps);
            }
        } else if (type == 9) {
            // Array — check string elements for capability hints
            uint32_t elem_type = 0;
            uint64_t count = 0;
            if (read_gguf_le(in, elem_type) && read_gguf_le(in, count)) {
                if (elem_type == 8) {
                    for (uint64_t j = 0; j < count; ++j) {
                        std::string value;
                        if (!read_gguf_string(in, value)) return false;
                        inspect_gguf_string(key, value, out.caps);
                    }
                } else if (elem_type != 9) {
                    uint64_t elem_size = gguf_scalar_size(elem_type);
                    if (elem_size == 0) return false;
                    if (!skip_gguf_bytes(in, count * elem_size)) return false;
                } else {
                    return false;
                }
            } else {
                return false;
            }
        } else {
            if (!skip_gguf_value(in, type)) return false;
        }
    }

    if (out.context_length == 0 && pending_context_length > 0) {
        out.context_length = pending_context_length;
    }

    // ── Derive scalar convenience fields from raw arrays ──────────────
    // head_count_kv: total KV heads across all blocks
    if (!out.head_count_kv_per_layer.empty()) {
        for (int64_t v : out.head_count_kv_per_layer)
            out.head_count_kv += v;
    } else if (out.head_count_kv_scalar > 0 && out.block_count > 0) {
        out.head_count_kv = out.head_count_kv_scalar * out.block_count;
        // Also populate the per-layer array for uniform scalar case
        out.head_count_kv_per_layer.assign(out.block_count, out.head_count_kv_scalar);
    }

    // swa_layer_count: number of layers with sliding-window attention
    if (!out.sliding_window_pattern.empty()) {
        for (bool v : out.sliding_window_pattern)
            if (v) out.swa_layer_count++;
    }

    return true;
}

/// Compute KV cache bytes-per-token using raw per-layer arrays from GGUF.
///
/// When head_count_kv_per_layer and sliding_window_pattern are populated,
/// this gives a precise weighted sum that accounts for per-layer head-count
/// variation combined with SWA-reduced key lengths.  When only scalar
/// metadata is available, falls back to the proportional approximation.
///
/// @param gguf       GGUF metadata with raw arrays populated
/// @param[out] scale_out  Output scaling factor (for logging). Set to 1.0 if
///                        no architecture-specific scaling applies. May be null.
/// @return Bytes per token for KV cache, or 0 if metadata is insufficient.
inline double compute_weighted_kv_cache_bytes_per_token(const GgufMetadata& gguf,
                                                        double* scale_out = nullptr) {
    int64_t block_count = gguf.block_count;
    int64_t key_length = gguf.key_length;
    int64_t key_length_swa = gguf.key_length_swa;

    if (block_count <= 0 || key_length <= 0)
        return 0.0;

    // Hybrid SSM-attention: only 1 every N layers grows KV cache;
    // the rest use SSM recurrent state (constant memory regardless of context).
    // Full-attention layers at indices 0, interval, 2*interval, …
    // Exact count: floor((block_count - 1) / interval) + 1
    if (gguf.full_attention_interval > 1) {
        int64_t full_attn_layers = (block_count - 1) / gguf.full_attention_interval + 1;
        double factor = static_cast<double>(full_attn_layers)
                      / static_cast<double>(block_count);
        if (scale_out) *scale_out = factor;
        int64_t total_heads = gguf.head_count_kv;
        if (total_heads <= 0)
            return 0.0;
        return static_cast<double>(total_heads)
             * static_cast<double>(key_length)
             * 2.0  // F16
             * 2.0  // K+V
             * factor;
    }

    // ── SWA or standard MHA/GQA ──────────────────────────────────────
    bool has_swa = (key_length_swa > 0 && key_length_swa < key_length);

    if (has_swa && !gguf.head_count_kv_per_layer.empty() && !gguf.sliding_window_pattern.empty()
        && gguf.head_count_kv_per_layer.size() == gguf.sliding_window_pattern.size()) {
        // Precise per-layer weighted sum using raw arrays.
        // Each layer contributes: heads[layer] × key_len[layer] × 2[F16] × 2[K+V]
        // where key_len[layer] = key_length_swa if SWA, key_length otherwise.
        size_t n = gguf.head_count_kv_per_layer.size();
        double weighted_sum = 0.0;
        double unweighted_sum = 0.0;

        for (size_t i = 0; i < n; ++i) {
            int64_t heads = gguf.head_count_kv_per_layer[i];
            if (heads <= 0) continue;
            double kl = gguf.sliding_window_pattern[i] ? key_length_swa : key_length;
            weighted_sum += static_cast<double>(heads) * kl;
            unweighted_sum += static_cast<double>(heads) * key_length;
        }

        if (weighted_sum > 0 && scale_out) {
            *scale_out = (unweighted_sum > 0) ? weighted_sum / unweighted_sum : 1.0;
        }

        return weighted_sum * 2.0 * 2.0;
    }

    // Scalar/uniform case: use proportional approximation.
    // This also covers the non-SWA standard MHA/GQA path.
    int64_t total_heads = gguf.head_count_kv;
    if (total_heads <= 0)
        return 0.0;

    double factor = 1.0;
    if (has_swa && gguf.swa_layer_count > 0) {
        double swa_ratio = static_cast<double>(gguf.swa_layer_count)
                         / static_cast<double>(block_count);
        double dim_ratio = static_cast<double>(key_length_swa)
                         / static_cast<double>(key_length);
        factor = 1.0 - swa_ratio + swa_ratio * dim_ratio;
        factor = (std::max)(0.1, factor);
    }

    if (scale_out) *scale_out = factor;

    return static_cast<double>(total_heads)
         * static_cast<double>(key_length)
         * 2.0  // F16
         * 2.0  // K+V
         * factor;
}

} // namespace lemon
