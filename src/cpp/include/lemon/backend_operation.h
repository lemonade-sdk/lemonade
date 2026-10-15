#pragma once

#include "lemon/model_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace lemon {

enum class BackendOperation : uint8_t {
    ChatCompletion,
    Completion,
    Responses,
    Embeddings,
    Reranking,
    Classification,
    Transcription,
    StreamingTranscription,
    TextToSpeech,
    AudioGeneration,
    ImageGeneration,
    ImageEdits,
    ImageVariations,
    Upscale,
    Model3DGeneration,
    Slots,
    SlotAction,
    Tokenize,
    Count
};

enum class OperationDelivery : uint8_t {
    Buffered,
    Streaming,
    Realtime,
    Count
};

enum class OperationProvenance : uint8_t {
    Implemented,
    LegacyForwarding
};

using ModelTypeMask = uint16_t;

inline constexpr std::array<ModelType, 9> k_operation_model_types = {
    ModelType::LLM, ModelType::EMBEDDING, ModelType::RERANKING,
    ModelType::TRANSCRIPTION, ModelType::IMAGE, ModelType::TTS,
    ModelType::AUDIO_GENERATION, ModelType::CLASSIFICATION, ModelType::MESH
};
inline constexpr size_t k_backend_operation_count = static_cast<size_t>(BackendOperation::Count);
inline constexpr size_t k_operation_delivery_count = static_cast<size_t>(OperationDelivery::Count);
inline constexpr ModelTypeMask k_all_operation_model_types = (1u << k_operation_model_types.size()) - 1;

constexpr ModelTypeMask operation_model_type_mask(ModelType model_type) noexcept {
    for (size_t i = 0; i < k_operation_model_types.size(); ++i) {
        if (k_operation_model_types[i] == model_type) {
            return static_cast<ModelTypeMask>(1u << i);
        }
    }
    return 0;
}

struct OperationQuery {
    BackendOperation operation;
    OperationDelivery delivery;
    ModelType model_type;
};

struct OperationDescription {
    BackendOperation operation;
    OperationDelivery delivery;
    ModelTypeMask model_types;
    OperationProvenance provenance;
};

struct OperationBinding {
    OperationDescription description;
    std::string_view handler;
    std::string_view codec;
};

enum class LegacyOperationProfile : uint8_t {
    LlamaCpp,
    Hrx,
    Ds4,
    RyzenAI,
    Vllm,
    Cloud,
    FastFlowLM,
    Whisper,
    Moonshine,
    Kokoro,
    StableDiffusion,
    TheNoise,
    OnnxRuntime,
    ThinkSound,
    AceStep,
    Trellis,
    OpenMoss,
    Count
};

void validate_operation_binding_signatures(const std::vector<OperationBinding>& bindings, bool compiled_compatibility);
std::string_view backend_operation_name(BackendOperation operation) noexcept;
bool parse_backend_operation(std::string_view name, BackendOperation& operation) noexcept;
bool valid_operation_delivery(BackendOperation operation, OperationDelivery delivery) noexcept;

} // namespace lemon
