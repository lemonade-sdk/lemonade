#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lemon {

constexpr const char* COLLECTION_OMNI_MODEL_RECIPE = "collection.omni";
constexpr const char* COLLECTION_ROUTER_MODEL_RECIPE = "collection.router";

inline bool is_omni_collection_recipe(const std::string& recipe) {
    return recipe == COLLECTION_OMNI_MODEL_RECIPE;
}

inline bool is_router_collection_recipe(const std::string& recipe) {
    return recipe == COLLECTION_ROUTER_MODEL_RECIPE;
}

inline bool is_model_collection_recipe(const std::string& recipe) {
    return is_omni_collection_recipe(recipe) || is_router_collection_recipe(recipe);
}

enum class ModelState {
    LOADING,
    READY,
    IN_USE,
    DOWNSIZING,
    DOWNSIZED,
    EVICTING,
    UNLOADED
};

inline std::string model_state_to_string(ModelState state) {
    switch (state) {
        case ModelState::LOADING: return "loading";
        case ModelState::READY: return "ready";
        case ModelState::IN_USE: return "in_use";
        case ModelState::DOWNSIZING: return "downsizing";
        case ModelState::DOWNSIZED: return "downsized";
        case ModelState::EVICTING: return "evicting";
        case ModelState::UNLOADED: return "unloaded";
        default: return "unknown";
    }
}

enum class ModelType {
    LLM,
    EMBEDDING,
    RERANKING,
    TRANSCRIPTION,
    IMAGE,
    TTS,
    AUDIO_GENERATION,  // text -> audio clip (music, sound effects)
    CLASSIFICATION,    // text -> {label: score} (router classifier models)
    MESH               // image -> 3D mesh (glTF-binary)
};

// Bitmask pattern for models that use multiple devices
enum DeviceType : uint32_t {
    DEVICE_NONE = 0,
    DEVICE_CPU  = 1 << 0,
    DEVICE_GPU  = 1 << 1,
    DEVICE_NPU  = 1 << 2
};

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

inline std::string model_type_to_string(ModelType type) {
    switch (type) {
        case ModelType::LLM: return "llm";
        case ModelType::EMBEDDING: return "embedding";
        case ModelType::RERANKING: return "reranking";
        case ModelType::TRANSCRIPTION: return "transcription";
        case ModelType::IMAGE: return "image";
        case ModelType::TTS: return "tts";
        case ModelType::AUDIO_GENERATION: return "audio-generation";
        case ModelType::CLASSIFICATION: return "classification";
        case ModelType::MESH: return "mesh";
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

// Determine model type from labels.
//
// Labels describe *capabilities* (what the model accepts or produces). ModelType
// describes the *deployment mode* we spawn the backend subprocess in (LLM chat,
// ASR, embedding, etc.) and the LRU bucket the router uses. These are different
// concepts.
//
// Label semantics:
//   "transcription"          → model can serve /audio/transcriptions (functional)
//   "realtime-transcription" → model supports WebSocket /realtime streaming
//   "chat-transcription"     → model accepts audio input in /chat/completions
//
// Resolution: chat-indicator labels win. The "transcription" label triggers
// ModelType::TRANSCRIPTION only when no chat indicator is present (pure Whisper).
// "chat-transcription" is an LLM input-modality label and does not change the
// deployment mode.
inline ModelType get_model_type_from_labels(const std::vector<std::string>& labels) {
    for (const auto& label : labels) {
        if (label == "vision" || label == "reasoning" ||
            label == "tool-calling" || label == "tools" ||
            label == "chat-transcription") {
            return ModelType::LLM;
        }
    }
    for (const auto& label : labels) {
        if (label == "embeddings" || label == "embedding") {
            return ModelType::EMBEDDING;
        }
        if (label == "reranking") {
            return ModelType::RERANKING;
        }
        if (label == "transcription") {
            return ModelType::TRANSCRIPTION;
        }
        if (label == "image") {
            return ModelType::IMAGE;
        }
        if (label == "tts") {
            return ModelType::TTS;
        }
        if (label == "audio-generation") {
            return ModelType::AUDIO_GENERATION;
        }
        if (label == "classification" || label == "classifier") {
            return ModelType::CLASSIFICATION;
        }
        if (label == "3d") {
            return ModelType::MESH;
        }
    }
    return ModelType::LLM;
}

// Fallback device type for recipes with no registered backend descriptor
// (collections and unknown recipes); the descriptor registry is authoritative.
inline DeviceType get_device_type_from_recipe(const std::string& recipe) {
    (void)recipe;
    return DEVICE_NONE;
}

} // namespace lemon
