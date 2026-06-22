#pragma once

/// GGUF binary format reader and metadata extraction.
///
/// Provides a single-pass reader over the GGUF KV-header that extracts
/// architecture name, context length, block count, embedding dimension,
/// attention head / key dimensions, and capability hints (vision, tool-calling,
/// MTP).  All helpers are static — no stateful reader class is needed because
/// the header is read sequentially in one shot.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <lemon/gguf_capabilities.h>
#include <lemon/utils/path_utils.h>

namespace lemon {

/// Metadata extracted from a GGUF file's KV header.
///
/// Stored inside ModelInfo for llama.cpp models so that auto-tune and other
/// subsystems can estimate KV-cache pressure and clamp context size.
struct GgufMetadata {
    std::string architecture;
    int64_t context_length = 0;
    int64_t block_count = 0;
    int64_t embedding_length = 0;
    int64_t head_count_kv = 0;    // total across all blocks (summed from array, or scalar × block_count)
    int64_t key_length = 0;
    int64_t key_length_swa = 0;   // SWA-reduced key length (Gemma4, etc.)
    int64_t swa_layer_count = 0;  // layers with sliding-window attention
    int64_t full_attention_interval = 0; // every Nth layer does full attention (Qwen SSM)
    GgufCapabilities caps;
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
                    // Array of integers: one entry per block — sum to get total KV heads
                    uint32_t elem_type = 0;
                    uint64_t count = 0;
                    if (read_gguf_le(in, elem_type) && read_gguf_le(in, count)) {
                        int64_t total = 0;
                        for (uint64_t j = 0; j < count; ++j) {
                            int64_t v = 0;
                            if (read_gguf_integer_value(in, elem_type, v) && v > 0)
                                total += v;
                        }
                        out.head_count_kv = total;
                    }
                } else {
                    // Scalar: per-block count, multiply by block_count for total
                    int64_t value = 0;
                    if (read_gguf_integer_value(in, type, value) && value > 0)
                        out.head_count_kv = value * out.block_count;
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
        // Count true entries → swa_layer_count.  Match by suffix so we don't
        // need the architecture prefix (key shape: arch.attention.sliding_window_pattern).
        if (key.size() > std::strlen(".sliding_window_pattern")
             && gguf_reader_detail::ends_with_ignore_case(key, ".sliding_window_pattern") && type == 9) {
            uint32_t elem_type = 0;
            uint64_t count = 0;
            if (read_gguf_le(in, elem_type) && read_gguf_le(in, count)) {
                if (elem_type == 7) {  // BOOL
                    for (uint64_t j = 0; j < count; ++j) {
                        uint8_t v = 0;
                        if (read_gguf_le(in, v) && v)
                            out.swa_layer_count++;
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
    return true;
}

} // namespace lemon
