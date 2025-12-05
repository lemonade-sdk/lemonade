#pragma once

#include <string>

namespace lemon {

// Model type classification for LRU cache management
enum class ModelType {
    LLM,        // Chat/completion models
    EMBEDDING,  // Embedding models
    RERANKING,  // Reranking models
    AUDIO       // Audio models (speech-to-text transcription)
};

// Device type flags for tracking hardware usage
// Uses bitmask pattern for models that use multiple devices (e.g., oga-hybrid)
enum DeviceType : uint32_t {
    DEVICE_NONE = 0,
    DEVICE_CPU  = 1 << 0,  // 0x01
    DEVICE_GPU  = 1 << 1,  // 0x02
    DEVICE_NPU  = 1 << 2   // 0x04
};

// Bitwise operators for DeviceType flags
inline DeviceType operator|(DeviceType a, DeviceType b) {
    return static_cast<DeviceType>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline DeviceType operator&(DeviceType a, DeviceType b) {
    return static_cast<DeviceType>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline DeviceType& operator|=(DeviceType& a, DeviceType b) {
    a = a | b;
    return a;
}

// Helper functions
inline std::string model_type_to_string(ModelType type) {
    switch (type) {
        case ModelType::LLM: return "llm";
        case ModelType::EMBEDDING: return "embedding";
        case ModelType::RERANKING: return "reranking";
        case ModelType::AUDIO: return "audio";
        default: return "unknown";
    }
}

inline std::string device_type_to_string(DeviceType device) {
    std::string result;
    if (device & DEVICE_CPU) {
        if (!result.empty()) result += "|";
        result += "cpu";
    }
    if (device & DEVICE_GPU) {
        if (!result.empty()) result += "|";
        result += "gpu";
    }
    if (device & DEVICE_NPU) {
        if (!result.empty()) result += "|";
        result += "npu";
    }
    if (result.empty()) result = "none";
    return result;
}

// Determine model type from labels
inline ModelType get_model_type_from_labels(const std::vector<std::string>& labels) {
    for (const auto& label : labels) {
        if (label == "embeddings" || label == "embedding") {
            return ModelType::EMBEDDING;
        }
        if (label == "reranking") {
            return ModelType::RERANKING;
        }
        if (label == "audio") {
            return ModelType::AUDIO;
        }
    }
    return ModelType::LLM;
}

// Determine device type from recipe
inline DeviceType get_device_type_from_recipe(const std::string& recipe) {
    if (recipe == "llamacpp") {
        return DEVICE_GPU;
    } else if (recipe == "oga-hybrid") {
        return DEVICE_GPU | DEVICE_NPU;
    } else if (recipe == "oga-npu") {
        return DEVICE_NPU;
    } else if (recipe == "oga-cpu") {
        return DEVICE_CPU;
    } else if (recipe == "flm") {
        return DEVICE_NPU;
    } else if (recipe == "whispercpp") {
        return DEVICE_CPU;  // Whisper.cpp runs on CPU (with optional GPU acceleration)
    }
    return DEVICE_NONE;
}

} // namespace lemon

