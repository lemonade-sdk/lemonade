#include "lemon/backend_operation.h"

namespace lemon {
namespace {
constexpr std::array<std::string_view, k_backend_operation_count> k_operation_names = {
    "chat_completion", "completion", "responses", "embeddings", "reranking", "classification",
    "transcription", "streaming_transcription", "text_to_speech", "audio_generation",
    "image_generation", "image_edits", "image_variations", "upscale", "model_3d_generation",
    "slots", "slot_action", "tokenize"
};

}

std::string_view backend_operation_name(BackendOperation operation) noexcept {
    return static_cast<size_t>(operation) < k_operation_names.size() ? k_operation_names[static_cast<size_t>(operation)] : "";
}

bool parse_backend_operation(std::string_view name, BackendOperation& operation) noexcept {
    for (size_t i = 0; i < k_operation_names.size(); ++i) {
        if (k_operation_names[i] == name) {
            operation = static_cast<BackendOperation>(i);
            return true;
        }
    }
    return false;
}

bool valid_operation_delivery(BackendOperation operation, OperationDelivery delivery) noexcept {
    if (static_cast<size_t>(operation) >= k_backend_operation_count ||
        static_cast<size_t>(delivery) >= k_operation_delivery_count) {
        return false;
    }
    if (operation == BackendOperation::StreamingTranscription) {
        return delivery == OperationDelivery::Realtime;
    }
    if (operation <= BackendOperation::Responses || operation == BackendOperation::TextToSpeech) {
        return delivery == OperationDelivery::Buffered || delivery == OperationDelivery::Streaming;
    }
    return delivery == OperationDelivery::Buffered;
}

}
