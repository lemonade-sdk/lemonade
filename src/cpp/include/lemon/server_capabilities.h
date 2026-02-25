#pragma once

#include <nlohmann/json.hpp>
#include <httplib.h>

namespace lemon {

using json = nlohmann::json;

// Base capability interface
class ICapability {
public:
    virtual ~ICapability() = default;
};

// Core completion capabilities that all servers must support
class ICompletionServer : public virtual ICapability {
public:
    virtual ~ICompletionServer() = default;
    virtual json chat_completion(const json& request) = 0;
    virtual json completion(const json& request) = 0;
};

// Optional embeddings capability
class IEmbeddingsServer : public virtual ICapability {
public:
    virtual ~IEmbeddingsServer() = default;
    virtual json embeddings(const json& request) = 0;
};

// Optional reranking capability
class IRerankingServer : public virtual ICapability {
public:
    virtual ~IRerankingServer() = default;
    virtual json reranking(const json& request) = 0;
};

// Optional audio capability (speech-to-text)
class IAudioServer : public virtual ICapability {
public:
    virtual ~IAudioServer() = default;

    // Speech-to-text transcription (OpenAI /v1/audio/transcriptions compatible)
    virtual json audio_transcriptions(const json& request) = 0;
};

// Optional audio capability (text-to-speech)
class ITextToSpeechServer : public virtual ICapability {
public:
    virtual ~ITextToSpeechServer() = default;

    // Speech-to-text transcription (OpenAI /v1/audio/speech compatible)
    virtual void audio_speech(const json& request, httplib::DataSink& sink) = 0;
};

// Optional image generation capability
class IImageServer : public virtual ICapability {
public:
    virtual ~IImageServer() = default;

    // Image generation (OpenAI /v1/images/generations compatible)
    virtual json image_generations(const json& request) = 0;
};

// Helper to check if a server supports a capability
template<typename T>
bool supports_capability(ICapability* server) {
    return dynamic_cast<T*>(server) != nullptr;
}

} // namespace lemon
