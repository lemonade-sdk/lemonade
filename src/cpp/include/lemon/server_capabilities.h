#pragma once

#include <nlohmann/json.hpp>
#include <httplib.h>

namespace lemon {

using json = nlohmann::json;

class ICapability {
public:
    virtual ~ICapability() = default;
};

class ICompletionServer : public virtual ICapability {
public:
    virtual ~ICompletionServer() = default;
    virtual json chat_completion(const json& request) = 0;
    virtual json completion(const json& request) = 0;
};

class IEmbeddingsServer : public virtual ICapability {
public:
    virtual ~IEmbeddingsServer() = default;
    virtual json embeddings(const json& request) = 0;
};

class IRerankingServer : public virtual ICapability {
public:
    virtual ~IRerankingServer() = default;
    virtual json reranking(const json& request) = 0;
};

class ITranscriptionServer : public virtual ICapability {
public:
    virtual ~ITranscriptionServer() = default;
    virtual json audio_transcriptions(const json& request) = 0;
};

class ITextToSpeechServer : public virtual ICapability {
public:
    virtual ~ITextToSpeechServer() = default;
    virtual void audio_speech(const json& request, httplib::DataSink& sink) = 0;
};

class IImageServer : public virtual ICapability {
public:
    virtual ~IImageServer() = default;
    virtual json image_generations(const json& request) = 0;
    virtual json image_edits(const json& request) = 0;
    virtual json image_variations(const json& request) = 0;
};

class ISlotsServer : public virtual ICapability {
public:
    virtual ~ISlotsServer() = default;
    virtual json get_slots() = 0;
    virtual json slots_action(int slot_id, const std::string& action, const json& request_body) = 0;
};

class ITokenizerServer : public virtual ICapability {
public:
    virtual ~ITokenizerServer() = default;
    virtual json tokenize(const json& request_body) = 0;
};

template<typename T>
bool supports_capability(ICapability* server) {
    return dynamic_cast<T*>(server) != nullptr;
}

} // namespace lemon
