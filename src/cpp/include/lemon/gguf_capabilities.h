#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <istream>
#include <limits>
#include <string>
#include <vector>

namespace lemon {

struct GgufCapabilities {
    bool vision = false;
    bool tool_calling = false;
};

namespace gguf_capabilities_detail {

template <typename T>
bool read_le(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

inline bool read_string(std::istream& in, std::string& value) {
    uint64_t len = 0;
    if (!read_le(in, len)) return false;
    if (len > 16 * 1024 * 1024) return false;
    value.assign(static_cast<size_t>(len), '\0');
    if (len == 0) return true;
    in.read(&value[0], static_cast<std::streamsize>(len));
    return static_cast<bool>(in);
}

inline bool skip_bytes(std::istream& in, uint64_t bytes) {
    if (bytes > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) return false;
    in.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
    return static_cast<bool>(in);
}

inline uint64_t scalar_size(uint32_t type) {
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

inline bool is_stable_vision_metadata_key(const std::string& key) {
    return key == "general.architecture" || key == "general.basename" ||
           key == "general.name" || key == "general.finetune" ||
           contains(key, "clip.vision") || contains(key, "mmproj");
}

inline bool is_chat_template_key(const std::string& key) {
    return key == "tokenizer.chat_template" || contains(key, ".chat_template");
}

inline void inspect_string(const std::string& key, const std::string& value, GgufCapabilities& caps) {
    const std::string k = to_lower(key);
    const std::string v = to_lower(value);

    // Vision is intentionally inferred only from stable metadata keys. Avoid
    // scanning arbitrary strings such as prompts or chat templates; those can
    // mention images/tools even for text-only or embedding models.
    if (is_stable_vision_metadata_key(k) &&
        (contains(v, "vision") || contains(v, "image") || contains(v, "mmproj") ||
         contains(v, "multimodal") || contains(v, "multi-modal") ||
         contains(v, "qwen2vl") || contains(v, "qwen2_5_vl") || contains(v, "qwen3vl") ||
         contains(v, "mllama") || contains(v, "llava") || contains(v, "pixtral") ||
         contains(v, "paligemma"))) {
        caps.vision = true;
    }

    // Tool support belongs in tokenizer.chat_template. Do not infer it from
    // arbitrary GGUF metadata keys or values.
    if (is_chat_template_key(k) &&
        (contains(v, "tool_call") || contains(v, "tool-call") ||
         contains(v, "function_call") || contains(v, "function-call") ||
         contains(v, "<tool") || contains(v, "</tool") ||
         contains(v, " tools") || contains(v, "\"tools\"") ||
         contains(v, "'tools'"))) {
        caps.tool_calling = true;
    }
}

inline bool skip_value(std::istream& in, uint32_t type);

inline bool read_or_skip_array(std::istream& in, const std::string& key, GgufCapabilities& caps) {
    uint32_t elem_type = 0;
    uint64_t count = 0;
    if (!read_le(in, elem_type) || !read_le(in, count)) return false;

    if (elem_type == 8) {
        for (uint64_t i = 0; i < count; ++i) {
            std::string value;
            if (!read_string(in, value)) return false;
            inspect_string(key, value, caps);
        }
        return true;
    }

    if (elem_type == 9) return false;
    uint64_t elem_size = scalar_size(elem_type);
    if (elem_size == 0) return false;
    if (count > std::numeric_limits<uint64_t>::max() / elem_size) return false;
    return skip_bytes(in, count * elem_size);
}

inline bool skip_value(std::istream& in, uint32_t type) {
    if (type == 8) {
        std::string ignored;
        return read_string(in, ignored);
    }
    if (type == 9) {
        GgufCapabilities ignored_caps;
        return read_or_skip_array(in, "", ignored_caps);
    }
    uint64_t size = scalar_size(type);
    return size > 0 && skip_bytes(in, size);
}

}  // namespace gguf_capabilities_detail

inline GgufCapabilities read_gguf_capabilities(std::istream& in) {
    using namespace gguf_capabilities_detail;

    GgufCapabilities caps;
    char magic[4] = {};
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, "GGUF", 4) != 0) return caps;

    uint32_t version = 0;
    uint64_t tensor_count = 0;
    uint64_t kv_count = 0;
    if (!read_le(in, version) || !read_le(in, tensor_count) || !read_le(in, kv_count)) return caps;
    (void)version;
    (void)tensor_count;

    for (uint64_t i = 0; i < kv_count; ++i) {
        std::string key;
        uint32_t type = 0;
        if (!read_string(in, key) || !read_le(in, type)) break;
        inspect_string(key, "", caps);

        if (type == 8) {
            std::string value;
            if (!read_string(in, value)) break;
            inspect_string(key, value, caps);
        } else if (type == 9) {
            if (!read_or_skip_array(in, key, caps)) break;
        } else if (!skip_value(in, type)) {
            break;
        }

        if (caps.vision && caps.tool_calling) break;
    }

    return caps;
}

inline bool add_label_once(std::vector<std::string>& labels, const std::string& label) {
    if (std::find(labels.begin(), labels.end(), label) != labels.end()) return false;
    labels.push_back(label);
    return true;
}

inline bool apply_gguf_capability_labels(std::vector<std::string>& labels, const GgufCapabilities& caps) {
    bool changed = false;
    if (caps.vision) changed = add_label_once(labels, "vision") || changed;
    if (caps.tool_calling) changed = add_label_once(labels, "tool-calling") || changed;
    return changed;
}

}  // namespace lemon
