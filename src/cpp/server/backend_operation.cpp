#include "lemon/backend_operation.h"
#include "lemon/operation_adapters.h"
#include "lemon/wrapped_server.h"

#include <stdexcept>
#include <utility>
#include <variant>

namespace lemon {
namespace {

using HandlerTarget = std::variant<
    decltype(&ICompletionServer::chat_completion),
    decltype(&WrappedServer::responses),
    decltype(&WrappedServer::forward_streaming_request),
    decltype(&IEmbeddingsServer::embeddings),
    decltype(&IRerankingServer::reranking),
    decltype(&IClassificationServer::classify),
    decltype(&ITranscriptionServer::audio_transcriptions),
    decltype(&IStreamingTranscriptionServer::get_streaming_address),
    decltype(&ITextToSpeechServer::audio_speech),
    decltype(&IAudioGenerationServer::audio_generations),
    decltype(&IImageServer::image_generations),
    decltype(&IUpscaleServer::upscale_via_cli),
    decltype(&IModel3DServer::model_3d_generations),
    decltype(&ISlotsServer::get_slots),
    decltype(&ISlotsServer::slots_action),
    decltype(&ITokenizerServer::tokenize)>;

struct HandlerRegistration {
    std::string_view id;
    BackendOperation operation;
    OperationDelivery delivery;
    std::string_view codec;
    HandlerTarget target;
};

constexpr HandlerRegistration k_handlers[] = {
    {"chat.buffered", BackendOperation::ChatCompletion, OperationDelivery::Buffered, "json", &ICompletionServer::chat_completion},
    {"chat.streaming", BackendOperation::ChatCompletion, OperationDelivery::Streaming, "sse", &WrappedServer::forward_streaming_request},
    {"completion.buffered", BackendOperation::Completion, OperationDelivery::Buffered, "json", &ICompletionServer::completion},
    {"completion.streaming", BackendOperation::Completion, OperationDelivery::Streaming, "sse", &WrappedServer::forward_streaming_request},
    {"responses.buffered", BackendOperation::Responses, OperationDelivery::Buffered, "json", &WrappedServer::responses},
    {"responses.streaming", BackendOperation::Responses, OperationDelivery::Streaming, "sse", &WrappedServer::forward_streaming_request},
    {"embeddings", BackendOperation::Embeddings, OperationDelivery::Buffered, "json", &IEmbeddingsServer::embeddings},
    {"reranking", BackendOperation::Reranking, OperationDelivery::Buffered, "json", &IRerankingServer::reranking},
    {"classification", BackendOperation::Classification, OperationDelivery::Buffered, "json", &IClassificationServer::classify},
    {"transcription", BackendOperation::Transcription, OperationDelivery::Buffered, "multipart", &ITranscriptionServer::audio_transcriptions},
    {"transcription.realtime", BackendOperation::StreamingTranscription, OperationDelivery::Realtime, "jsonl", &IStreamingTranscriptionServer::get_streaming_address},
    {"speech.buffered", BackendOperation::TextToSpeech, OperationDelivery::Buffered, "media", &ITextToSpeechServer::audio_speech},
    {"speech.streaming", BackendOperation::TextToSpeech, OperationDelivery::Streaming, "media", &ITextToSpeechServer::audio_speech},
    {"audio", BackendOperation::AudioGeneration, OperationDelivery::Buffered, "media", &IAudioGenerationServer::audio_generations},
    {"image", BackendOperation::ImageGeneration, OperationDelivery::Buffered, "json", &IImageServer::image_generations},
    {"image.edits", BackendOperation::ImageEdits, OperationDelivery::Buffered, "json", &IImageServer::image_edits},
    {"image.variations", BackendOperation::ImageVariations, OperationDelivery::Buffered, "json", &IImageServer::image_variations},
    {"upscale", BackendOperation::Upscale, OperationDelivery::Buffered, "base64", &IUpscaleServer::upscale_via_cli},
    {"mesh", BackendOperation::Model3DGeneration, OperationDelivery::Buffered, "media", &IModel3DServer::model_3d_generations},
    {"slots", BackendOperation::Slots, OperationDelivery::Buffered, "json", &ISlotsServer::get_slots},
    {"slots.action", BackendOperation::SlotAction, OperationDelivery::Buffered, "json", &ISlotsServer::slots_action},
    {"tokenize", BackendOperation::Tokenize, OperationDelivery::Buffered, "json", &ITokenizerServer::tokenize}
};

template<class Target>
bool target_is(const HandlerTarget& registered, Target target) {
    const auto* resolved = std::get_if<Target>(&registered);
    return resolved && *resolved == target;
}

bool handler_signature_matches(const HandlerRegistration& handler) {
    if (handler.delivery == OperationDelivery::Streaming && handler.operation <= BackendOperation::Responses) {
        return target_is(handler.target, &WrappedServer::forward_streaming_request);
    }
    switch (handler.operation) {
        case BackendOperation::ChatCompletion: return target_is(handler.target, &ICompletionServer::chat_completion);
        case BackendOperation::Completion: return target_is(handler.target, &ICompletionServer::completion);
        case BackendOperation::Responses: return target_is(handler.target, &WrappedServer::responses);
        case BackendOperation::Embeddings: return target_is(handler.target, &IEmbeddingsServer::embeddings);
        case BackendOperation::Reranking: return target_is(handler.target, &IRerankingServer::reranking);
        case BackendOperation::Classification: return target_is(handler.target, &IClassificationServer::classify);
        case BackendOperation::Transcription: return target_is(handler.target, &ITranscriptionServer::audio_transcriptions);
        case BackendOperation::StreamingTranscription: return target_is(handler.target, &IStreamingTranscriptionServer::get_streaming_address);
        case BackendOperation::TextToSpeech: return target_is(handler.target, &ITextToSpeechServer::audio_speech);
        case BackendOperation::AudioGeneration: return target_is(handler.target, &IAudioGenerationServer::audio_generations);
        case BackendOperation::ImageGeneration: return target_is(handler.target, &IImageServer::image_generations);
        case BackendOperation::ImageEdits: return target_is(handler.target, &IImageServer::image_edits);
        case BackendOperation::ImageVariations: return target_is(handler.target, &IImageServer::image_variations);
        case BackendOperation::Upscale: return target_is(handler.target, &IUpscaleServer::upscale_via_cli);
        case BackendOperation::Model3DGeneration: return target_is(handler.target, &IModel3DServer::model_3d_generations);
        case BackendOperation::Slots: return target_is(handler.target, &ISlotsServer::get_slots);
        case BackendOperation::SlotAction: return target_is(handler.target, &ISlotsServer::slots_action);
        case BackendOperation::Tokenize: return target_is(handler.target, &ITokenizerServer::tokenize);
        default: return false;
    }
}

bool has_adapter(const OperationAdapters& adapters, BackendOperation operation,
                 OperationDelivery delivery, OperationProvenance provenance) {
    switch (operation) {
        case BackendOperation::ChatCompletion:
        case BackendOperation::Completion:
        case BackendOperation::Responses:
            if (delivery == OperationDelivery::Streaming) {
                return adapters.streaming_forwarder &&
                    (provenance == OperationProvenance::LegacyForwarding ||
                     adapters.completion_implementations[static_cast<size_t>(operation)]);
            }
            return adapters.completion && adapters.completion_implementations[static_cast<size_t>(operation)] &&
                (operation != BackendOperation::Responses || adapters.responses);
        case BackendOperation::Embeddings: return adapters.embeddings;
        case BackendOperation::Reranking: return adapters.reranking;
        case BackendOperation::Classification: return adapters.classification;
        case BackendOperation::Transcription: return adapters.transcription;
        case BackendOperation::StreamingTranscription: return adapters.streaming_transcription;
        case BackendOperation::TextToSpeech: return adapters.text_to_speech;
        case BackendOperation::AudioGeneration: return adapters.audio_generation;
        case BackendOperation::ImageGeneration:
        case BackendOperation::ImageEdits:
        case BackendOperation::ImageVariations: return adapters.image;
        case BackendOperation::Upscale: return adapters.upscale;
        case BackendOperation::Model3DGeneration: return adapters.model_3d;
        case BackendOperation::Slots:
        case BackendOperation::SlotAction: return adapters.slots;
        case BackendOperation::Tokenize: return adapters.tokenizer;
        default: return false;
    }
}

const HandlerRegistration& handler_for(std::string_view id) {
    for (const auto& handler : k_handlers) {
        if (handler.id == id) {
            return handler;
        }
    }
    throw std::invalid_argument("Unknown operation handler");
}

void validate_description(const OperationDescription& description, bool compiled_compatibility) {
    if (!valid_operation_delivery(description.operation, description.delivery) ||
        !description.model_types || (description.model_types & ~k_all_operation_model_types)) {
        throw std::invalid_argument("Invalid operation, delivery or model types");
    }
    if (description.provenance != OperationProvenance::Implemented &&
        description.provenance != OperationProvenance::LegacyForwarding) {
        throw std::invalid_argument("Invalid operation provenance");
    }
    if (description.provenance == OperationProvenance::LegacyForwarding &&
        (!compiled_compatibility || description.delivery != OperationDelivery::Streaming ||
         description.operation > BackendOperation::Responses)) {
        throw std::invalid_argument("Legacy forwarding requires a compiled SSE compatibility binding");
    }
}

const HandlerRegistration& validate_binding_signature(const OperationBinding& binding, bool compiled_compatibility) {
    const auto& description = binding.description;
    validate_description(description, compiled_compatibility);
    const auto& handler = handler_for(binding.handler);
    if (handler.operation != description.operation || handler.delivery != description.delivery || !handler_signature_matches(handler)) throw std::invalid_argument("Operation handler signature mismatch");
    if (binding.codec != handler.codec) throw std::invalid_argument("Unknown or incompatible operation codec");
    if (!std::visit([](auto target) { return target != nullptr; }, handler.target)) throw std::invalid_argument("Operation handler has no executable adapter");
    return handler;
}

}

void validate_operation_binding_signatures(const std::vector<OperationBinding>& bindings, bool compiled_compatibility) {
    for (size_t i = 0; i < bindings.size(); ++i) {
        validate_binding_signature(bindings[i], compiled_compatibility);
        const auto& current = bindings[i].description;
        for (size_t j = 0; j < i; ++j) {
            const auto& previous = bindings[j].description;
            if (previous.operation == current.operation && previous.delivery == current.delivery && (previous.model_types & current.model_types)) throw std::invalid_argument("Overlapping operation bindings");
        }
    }
}

OperationBindings::OperationBindings(OperationAdapters adapters, std::vector<OperationBinding> bindings,
                                     bool compiled_compatibility,
                                     std::vector<OperationDescription> rejections)
    : adapters_(adapters), compiled_compatibility_(compiled_compatibility) {
    for (const auto& binding : bindings) {
        const auto& description = binding.description;
        validate_binding_signature(binding, compiled_compatibility);
        if (!has_adapter(adapters, description.operation, description.delivery, description.provenance)) {
            throw std::invalid_argument("Operation handler has no executable adapter");
        }
        for (size_t i = 0; i < k_operation_model_types.size(); ++i) {
            if (description.model_types & (1u << i)) {
                auto& cell = support_[static_cast<size_t>(description.operation)][static_cast<size_t>(description.delivery)][i];
                if (cell) {
                    throw std::invalid_argument("Overlapping operation bindings");
                }
                cell = true;
            }
        }
        descriptions_.push_back(description);
    }
    for (const auto& rejection : rejections) {
        validate_description(rejection, compiled_compatibility);
        if (!compiled_compatibility ||
            !has_adapter(adapters, rejection.operation, rejection.delivery, rejection.provenance)) {
            throw std::invalid_argument("Rejection requires a compiled adapter");
        }
        for (size_t i = 0; i < k_operation_model_types.size(); ++i) {
            if (rejection.model_types & (1u << i)) {
                auto& cell = rejections_[static_cast<size_t>(rejection.operation)][static_cast<size_t>(rejection.delivery)][i];
                if (cell || supports({rejection.operation, rejection.delivery, k_operation_model_types[i]})) {
                    throw std::invalid_argument("Overlapping operation rejection");
                }
                cell = true;
            }
        }
    }
}

bool OperationBindings::lookup(const Table& table, OperationQuery query) noexcept {
    if (static_cast<size_t>(query.operation) >= k_backend_operation_count ||
        static_cast<size_t>(query.delivery) >= k_operation_delivery_count) {
        return false;
    }
    for (size_t i = 0; i < k_operation_model_types.size(); ++i) {
        if (k_operation_model_types[i] == query.model_type) {
            return table[static_cast<size_t>(query.operation)][static_cast<size_t>(query.delivery)][i];
        }
    }
    return false;
}

bool OperationBindings::supports(OperationQuery query) const noexcept {
    return lookup(support_, query);
}

bool OperationBindings::has_rejection(OperationQuery query) const noexcept {
    return lookup(rejections_, query);
}

OperationBindings legacy_operation_bindings(LegacyOperationProfile profile, OperationAdapters adapters) {
    if (profile >= LegacyOperationProfile::Count) {
        throw std::invalid_argument("Unknown legacy operation profile");
    }
    std::vector<OperationBinding> bindings;
    std::vector<OperationDescription> rejections;
    auto add = [&](std::string_view id, ModelTypeMask modes = k_all_operation_model_types,
                   OperationProvenance provenance = OperationProvenance::Implemented) {
        const auto& handler = handler_for(id);
        bindings.push_back({{handler.operation, handler.delivery, modes, provenance}, id, handler.codec});
    };
    const auto transcription = operation_model_type_mask(ModelType::TRANSCRIPTION);
    const auto embedding = operation_model_type_mask(ModelType::EMBEDDING);
    const auto tts = operation_model_type_mask(ModelType::TTS);
    const auto audio = operation_model_type_mask(ModelType::AUDIO_GENERATION);
    const bool chat_backend = profile <= LegacyOperationProfile::FastFlowLM;
    const auto chat_modes = profile == LegacyOperationProfile::FastFlowLM
        ? static_cast<ModelTypeMask>(k_all_operation_model_types & ~(transcription | embedding))
        : k_all_operation_model_types;
    if (chat_backend) {
        add("chat.buffered", chat_modes);
        add("completion.buffered", chat_modes);
        if (profile != LegacyOperationProfile::Cloud && profile != LegacyOperationProfile::FastFlowLM) {
            add("responses.buffered");
        }
    }
    for (const auto id : {"chat.streaming", "completion.streaming", "responses.streaming"}) {
        const bool implemented = chat_backend &&
            (std::string_view(id) != "responses.streaming" ||
             (profile != LegacyOperationProfile::Cloud && profile != LegacyOperationProfile::FastFlowLM));
        add(id, chat_modes, implemented ? OperationProvenance::Implemented : OperationProvenance::LegacyForwarding);
    }
    switch (profile) {
        case LegacyOperationProfile::LlamaCpp:
            for (const auto id : {"embeddings", "reranking", "slots", "slots.action", "tokenize"}) add(id);
            break;
        case LegacyOperationProfile::FastFlowLM:
            add("embeddings", k_all_operation_model_types & ~transcription);
            add("transcription", transcription);
            rejections.push_back({BackendOperation::Embeddings, OperationDelivery::Buffered, transcription, OperationProvenance::Implemented});
            rejections.push_back({BackendOperation::Transcription, OperationDelivery::Buffered,
                                  static_cast<ModelTypeMask>(k_all_operation_model_types & ~transcription), OperationProvenance::Implemented});
            for (const auto operation : {BackendOperation::ChatCompletion, BackendOperation::Completion, BackendOperation::Responses}) {
                rejections.push_back({operation, OperationDelivery::Streaming,
                                      static_cast<ModelTypeMask>(transcription | embedding), OperationProvenance::LegacyForwarding});
            }
            break;
        case LegacyOperationProfile::Whisper: add("transcription"); break;
        case LegacyOperationProfile::Moonshine:
            add("transcription");
            add("transcription.realtime");
            break;
        case LegacyOperationProfile::Kokoro:
            add("speech.buffered", tts);
            add("speech.streaming", tts);
            break;
        case LegacyOperationProfile::StableDiffusion:
            for (const auto id : {"image", "image.edits", "image.variations", "upscale"}) add(id);
            break;
        case LegacyOperationProfile::TheNoise:
            for (const auto id : {"image", "image.edits", "upscale"}) add(id);
            rejections.push_back({BackendOperation::ImageVariations, OperationDelivery::Buffered,
                                  k_all_operation_model_types, OperationProvenance::Implemented});
            break;
        case LegacyOperationProfile::OnnxRuntime: add("classification"); break;
        case LegacyOperationProfile::ThinkSound:
        case LegacyOperationProfile::AceStep: add("audio", audio); break;
        case LegacyOperationProfile::Trellis: add("mesh"); break;
        case LegacyOperationProfile::OpenMoss:
            add("speech.buffered", tts);
            add("speech.streaming", tts);
            add("audio", audio);
            break;
        default: break;
    }
    return OperationBindings(adapters, std::move(bindings), true, std::move(rejections));
}

}
