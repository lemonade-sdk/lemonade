#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
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
// concepts, and only the labels below name a deployment mode. "vision",
// "reasoning", "tool-calling", "chat-transcription" and friends describe what a
// chat model accepts or how it behaves; they never make one. In particular
// "chat-transcription" is an input modality — the model takes audio in a chat
// turn and transcribes it as part of its answer — so a model carrying it is a
// chat model and says so with "chat", exactly as a vision model does.
//
// "chat" is checked first so an omni model that also declares a mode it could
// serve standalone ("transcription") still deploys as an LLM rather than as ASR.
//
// Templated on the container so a label set already held as a std::set can be
// tested without copying it into a vector.
template <typename Labels>
inline bool find_deployment_mode(const Labels& labels, ModelType& out) {
    for (const auto& label : labels) {
        if (label == "chat") {
            out = ModelType::LLM;
            return true;
        }
    }
    for (const auto& label : labels) {
        if (label == "embeddings" || label == "embedding") {
            out = ModelType::EMBEDDING;
            return true;
        }
        if (label == "reranking") {
            out = ModelType::RERANKING;
            return true;
        }
        if (label == "transcription") {
            out = ModelType::TRANSCRIPTION;
            return true;
        }
        if (label == "image") {
            out = ModelType::IMAGE;
            return true;
        }
        if (label == "tts") {
            out = ModelType::TTS;
            return true;
        }
        if (label == "audio-generation") {
            out = ModelType::AUDIO_GENERATION;
            return true;
        }
        if (label == "classification" || label == "classifier") {
            out = ModelType::CLASSIFICATION;
            return true;
        }
        if (label == "3d") {
            out = ModelType::MESH;
            return true;
        }
    }
    return false;
}

// Single-label form of find_deployment_mode(), for callers testing labels one at
// a time. Allocates nothing.
inline bool deployment_mode_of(std::string_view label, ModelType& out) {
    const std::array<std::string_view, 1> one{label};
    return find_deployment_mode(one, out);
}

// A label set that names no deployment mode stays usable as a chat model rather
// than failing to route. Use find_deployment_mode() where the difference between
// "declares chat" and "declares nothing" matters.
inline ModelType get_model_type_from_labels(const std::vector<std::string>& labels) {
    ModelType type = ModelType::LLM;
    find_deployment_mode(labels, type);
    return type;
}

inline bool has_label(const std::vector<std::string>& labels, const std::string& label) {
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}

inline bool add_label_once(std::vector<std::string>& labels, const std::string& label) {
    if (has_label(labels, label)) return false;
    labels.push_back(label);
    return true;
}

// Fallback device type for recipes with no registered backend descriptor
// (collections and unknown recipes); the descriptor registry is authoritative.
inline DeviceType get_device_type_from_recipe(const std::string& recipe) {
    (void)recipe;
    return DEVICE_NONE;
}

} // namespace lemon
