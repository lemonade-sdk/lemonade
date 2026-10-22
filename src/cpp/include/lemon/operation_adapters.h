#pragma once

#include "lemon/backend_operation.h"
#include "lemon/server_capabilities.h"
#include "lemon/streaming_proxy.h"

#include <array>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace lemon {

class WrappedServer;

struct OperationAdapters {
    using JsonHandler = json (*)(WrappedServer&, const json&);
    using StreamHandler = void (*)(WrappedServer&, const std::string&, const std::string&,
                                   httplib::DataSink&, bool, long,
                                   std::function<void(const StreamingProxy::TelemetryData&)>);

    ICompletionServer* completion = nullptr;
    IEmbeddingsServer* embeddings = nullptr;
    IRerankingServer* reranking = nullptr;
    IClassificationServer* classification = nullptr;
    ITranscriptionServer* transcription = nullptr;
    IStreamingTranscriptionServer* streaming_transcription = nullptr;
    ITextToSpeechServer* text_to_speech = nullptr;
    IImageServer* image = nullptr;
    IUpscaleServer* upscale = nullptr;
    IAudioGenerationServer* audio_generation = nullptr;
    IModel3DServer* model_3d = nullptr;
    ISlotsServer* slots = nullptr;
    ITokenizerServer* tokenizer = nullptr;
    std::array<bool, 3> completion_implementations{};
    JsonHandler responses = nullptr;
    std::array<JsonHandler, 3> completion_rejections{};
    StreamHandler streaming_forwarder = nullptr;

    template<class Interface>
    Interface* get(BackendOperation operation) const noexcept {
        if constexpr (std::is_same_v<Interface, ICompletionServer>) {
            return operation == BackendOperation::ChatCompletion ||
                   operation == BackendOperation::Completion ? completion : nullptr;
        } else if constexpr (std::is_same_v<Interface, IEmbeddingsServer>) {
            return operation == BackendOperation::Embeddings ? embeddings : nullptr;
        } else if constexpr (std::is_same_v<Interface, IRerankingServer>) {
            return operation == BackendOperation::Reranking ? reranking : nullptr;
        } else if constexpr (std::is_same_v<Interface, IClassificationServer>) {
            return operation == BackendOperation::Classification ? classification : nullptr;
        } else if constexpr (std::is_same_v<Interface, ITranscriptionServer>) {
            return operation == BackendOperation::Transcription ? transcription : nullptr;
        } else if constexpr (std::is_same_v<Interface, IStreamingTranscriptionServer>) {
            return operation == BackendOperation::StreamingTranscription ? streaming_transcription : nullptr;
        } else if constexpr (std::is_same_v<Interface, ITextToSpeechServer>) {
            return operation == BackendOperation::TextToSpeech ? text_to_speech : nullptr;
        } else if constexpr (std::is_same_v<Interface, IImageServer>) {
            return operation == BackendOperation::ImageGeneration || operation == BackendOperation::ImageEdits ||
                   operation == BackendOperation::ImageVariations ? image : nullptr;
        } else if constexpr (std::is_same_v<Interface, IUpscaleServer>) {
            return operation == BackendOperation::Upscale ? upscale : nullptr;
        } else if constexpr (std::is_same_v<Interface, IAudioGenerationServer>) {
            return operation == BackendOperation::AudioGeneration ? audio_generation : nullptr;
        } else if constexpr (std::is_same_v<Interface, IModel3DServer>) {
            return operation == BackendOperation::Model3DGeneration ? model_3d : nullptr;
        } else if constexpr (std::is_same_v<Interface, ISlotsServer>) {
            return operation == BackendOperation::Slots || operation == BackendOperation::SlotAction ? slots : nullptr;
        } else if constexpr (std::is_same_v<Interface, ITokenizerServer>) {
            return operation == BackendOperation::Tokenize ? tokenizer : nullptr;
        } else {
            static_assert(!std::is_same_v<Interface, Interface>, "Unknown operation adapter interface");
        }
    }
};

class OperationBindings {
public:
    OperationBindings(OperationAdapters adapters, std::vector<OperationBinding> bindings,
                      bool compiled_compatibility = false,
                      std::vector<OperationDescription> rejections = {});

    bool supports(OperationQuery query) const noexcept;
    bool has_rejection(OperationQuery query) const noexcept;
    const std::vector<OperationDescription>& descriptions() const noexcept { return descriptions_; }
    const OperationAdapters& adapters() const noexcept { return adapters_; }
    bool compiled_compatibility() const noexcept { return compiled_compatibility_; }

private:
    using Table = std::array<std::array<std::array<bool, k_operation_model_types.size()>,
                                       k_operation_delivery_count>, k_backend_operation_count>;
    static bool lookup(const Table& table, OperationQuery query) noexcept;
    const OperationAdapters adapters_;
    const bool compiled_compatibility_;
    Table support_{};
    Table rejections_{};
    std::vector<OperationDescription> descriptions_;
};

OperationBindings legacy_operation_bindings(LegacyOperationProfile profile, OperationAdapters adapters);

}
